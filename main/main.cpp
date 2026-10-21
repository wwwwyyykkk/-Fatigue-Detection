/**
 * FatigueGuard - ESP-IDF Fatigue Detection with Face Recognition
 * USB UVC camera (MJPEG) + Face Detection + Eye ROI + PERCLOS + Face Recognition
 */

#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "sdkconfig.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_task_wdt.h"

#include "usb/usb_host.h"
#include "usb/uvc_host.h"

#include "esp_jpeg_dec.h"

#include "human_face_detect.hpp"
#include "eye_roi.hpp"
#include "mouth_roi.hpp"
#include "perclos.hpp"

#include "eye_classifier_infer.h"
#include "face_recognizer.h"
#include "cloud_face.h"

/* ── Baidu AI credentials (cloud face recognition) ──
 * Replace these placeholders locally. Do not commit real keys. */
#define BAIDU_API_KEY    "YOUR_BAIDU_API_KEY"
#define BAIDU_SECRET_KEY "YOUR_BAIDU_SECRET_KEY"

// #include "voice_prompter.hpp"   /* I2S disabled — voice via VirtualGPS SYN6288 */
// #include "audio_resources.h"

#include "display_data.h"
#include "lvgl_ui.h"
#include "gps_uart.h"
#include "mic_test.h"
#include "esp_sr_task.h"

/* ESP-NOW / WiFi */
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

/* SNTP for wall-clock time */
#include "esp_sntp.h"

/* Global display data structure (updated by frame_handling_task, read by TFT task) */
display_data_t g_display_data = {
    .perclos = 0.0f,
    .fatigue_level = 0,
    .face_detected = false,
    .face_w = 0,
    .face_h = 0,
    .total_frames = 0,
    .face_frames = 0,
    .closed_frames = 0,
    .valid_count = 0,
    .is_warm = false,
    .driver_id = -1,
    .driver_similarity = 0.0f,
    .driving_minutes = 0,
    .weather_risk = 0,
    .drive_warn_min = 0,
    .gps_speed = 0.0f,
    .gps_location = 0,
    .gps_driver_id = 0,
    .gps_drive_seconds = 0,
    .gps_lat = 0.0f,
    .gps_lng = 0.0f,
    .gps_connected = false,
    .identifying = false,
    .camera_ready = false,
    .time_year = 2026,
    .time_month = 1,
    .time_day = 1,
    .time_valid = false,
    .pred_ready = false,
};

/* ── Multi-driver tracking ─────────────────────────────────── */
static int       s_current_driver_id = -1;   // Active driver, -1 = unknown
static uint32_t  s_driving_start_ms = 0;     // Session start timestamp
static int       s_face_lost_frames = 0;      // Consecutive no-face frames
static bool      s_force_reidentify = false;  // Manual re-identify trigger
static WeatherRisk s_weather_risk = WeatherRisk::CLEAR;  // Weather-linked threshold adjustment
int        g_weather_risk_raw = 0;     // Weather risk from G-prefix (0/1/2, written by gps_uart.cpp)
int        g_weather_pending  = 0;     // Signal to sync weather in main loop (set by gps_uart.cpp)
extern int g_rest_request_pending;     // Rest request from VirtualGPS BUTTON_3 (set by gps_uart.cpp)
extern int g_rest_stop_pending;        // Rest stop request from VirtualGPS BUTTON_3 (toggle)
extern int g_demo_cmd_pending;         // Demo Mode from VirtualGPS Rainmaker (1=start, 0=stop)

/* ── Detection state machine ─────────────────────────────── */
enum class DetectState : uint8_t {
    IDENTIFYING,   // Face recognition only, no PERCLOS
    MONITORING     // Normal fatigue detection (PERCLOS running)
};
static DetectState s_detect_state = DetectState::IDENTIFYING;
static uint32_t s_identify_start_ms = 0;          // When we entered IDENTIFYING state
static int      s_last_identify_event_sent = IDENTIFY_EVENT_NONE;
#define IDENTIFY_TIMEOUT_MS  15000                // 15s no face → timeout

/* ── F-prefix heartbeat state tracking ────────────────────── */
static int  s_last_fatigue     = 0;
static int  s_last_face_ok     = -1;
static int  s_last_drv_conf    = -1;
static int  s_last_sys_status  = 0;
static uint32_t s_last_f_send_ms = 0;
static uint32_t s_last_eg800k_send_ms = 0;

/* Demo ramp (VirtualGPS RainMaker Demo) — 需在 check_send_f_prefix 前声明 */
static bool s_demo_ramp_active = false;

static const char *TAG = "Fatigue";

#define USB_HOST_PRIORITY       (15)
#define FRAME_QUEUE_LEN         (30)

static QueueHandle_t s_frame_queue;
static bool          s_dev_connected = false;
static SemaphoreHandle_t s_usb_free_sem = NULL;   /* USB all-free sync for reconnect */
static HumanFaceDetect *s_detector = nullptr;

static bool s_face_recognized = false;
static int  s_pending_enroll_id = 0;
static bool s_face_recog_available = false;

/* ── Rest mode ──────────────────────────────────────────────── */
static bool     s_rest_active = false;
static int      s_rest_total_min = 15;
static uint32_t s_rest_start_ms = 0;
static uint32_t s_rest_result_show_ms = 0;  /* timestamp to show recovery result before switching */

/* Start rest mode — MUST be stopped & in a service area (speed==0, location==1)
 * If minutes <= 0: auto-detect from fatigue level (mild→10, severe→20, normal→15) */
static bool start_rest(int minutes)
{
    if (s_rest_active) { ESP_LOGW(TAG, "Already in rest mode"); return false; }

    float spd = g_display_data.gps_connected ? g_display_data.gps_speed : -1.0f;
    int   loc = g_display_data.gps_location;

    if (!g_display_data.gps_connected) {
        ESP_LOGW(TAG, "REST DENIED: VirtualGPS not connected (no GPS data)");
        return false;
    }
    if (spd > 0.0f) {
        ESP_LOGW(TAG, "REST DENIED: speed=%.1f km/h (need 0, parked)", spd);
        return false;
    }
    if (loc != 1) {
        ESP_LOGW(TAG, "REST DENIED: location=%d (need 1=service_area)", loc);
        return false;
    }

    /* Auto-detect rest duration from fatigue level */
    int lv = (int)g_display_data.fatigue_level;
    bool mandatory = false;
    if (minutes <= 0) {
        if (lv == 2)      { minutes = 20; mandatory = true; }
        else if (lv == 1) { minutes = 10; mandatory = false; }
        else              { minutes = 15; mandatory = false; }
    }

    s_rest_active = true;
    s_rest_total_min = minutes;
    s_rest_start_ms = esp_timer_get_time() / 1000;
    g_display_data.rest_active = true;
    g_display_data.rest_total_minutes = minutes;
    g_display_data.rest_elapsed_seconds = 0;
    g_display_data.rest_snap_fatigue = g_display_data.fatigue_level;
    g_display_data.rest_snap_perclos = g_display_data.perclos;
    g_display_data.rest_snap_drive_min = (int)g_display_data.driving_minutes;
    g_display_data.rest_recovery_pct = -1;     /* not computed yet */
    g_display_data.rest_mandatory = mandatory;
    g_display_data.rest_suggested_min = minutes;
    g_display_data.rest_face_ok = false;
    g_display_data.rest_extend_count = 0;
    s_last_identify_event_sent = IDENTIFY_EVENT_REST_START;

    ESP_LOGI(TAG, "╔══════════════════════════════╗");
    ESP_LOGI(TAG, "║  REST START: %d min %s     ║", minutes, mandatory ? "(MANDATORY)" : "(suggested)");
    ESP_LOGI(TAG, "║  fatigue=%s perclos=%.1f%%   ║",
             fatigue_level_str((FatigueLevel)lv), g_display_data.rest_snap_perclos);
    ESP_LOGI(TAG, "╚══════════════════════════════╝");
    return true;
}

static void stop_rest(bool completed)
{
    s_rest_active = false;
    g_display_data.rest_active = false;
    s_last_identify_event_sent = IDENTIFY_EVENT_REST_DONE;  /* always send DONE so VirtualGPS unlocks */
    if (!completed) {
        g_display_data.rest_recovery_pct = -2;   /* -2 = manually stopped, skip post-rest */
        g_display_data.rest_face_ok = true;      /* skip face re-verify */
    }
    ESP_LOGI(TAG, ">>> REST MODE ENDED (%s) <<<", completed ? "completed" : "stopped");
}

/* Send F-prefix when state changes (or every 5s heartbeat) */
static void check_send_f_prefix(void)
{
    uint32_t now = esp_timer_get_time() / 1000;
    /* In IDENTIFYING state: force face_ok=0, drv_conf=0, fatigue=0 */
    int face_ok = (s_detect_state == DetectState::MONITORING && s_face_recognized) ? 1 : 0;
    int drv_conf = (s_detect_state == DetectState::MONITORING && s_current_driver_id > 0) ? s_current_driver_id : 0;
    int sys_status = s_dev_connected ? 1 : 2;
    /* Only report real fatigue when window is warm (30+ samples).
     * Demo ramp 激活时直接发 ramp 的疲劳等级(否则 IDENTIFYING 状态强制 0,VirtualGPS 不播语音) */
    int fatigue = 0;
    if (s_demo_ramp_active) {
        fatigue = (int)g_display_data.fatigue_level;
    } else if (s_detect_state == DetectState::MONITORING && g_display_data.is_warm) {
        fatigue = (int)g_display_data.fatigue_level;
    }
    int id_evt = s_last_identify_event_sent;

    bool changed = (fatigue != s_last_fatigue) ||
                   (face_ok != s_last_face_ok) ||
                   (drv_conf != s_last_drv_conf) ||
                   (sys_status != s_last_sys_status) ||
                   (id_evt != IDENTIFY_EVENT_NONE);
    bool heartbeat = (now - s_last_f_send_ms > 5000);

    if (changed || heartbeat) {
        /* 预测目标等级: 当前>=1级 → 预测严重(30%), 否则预测轻度(15%) */
        int pred_level = (g_display_data.fatigue_level >= 1) ? 2 : 1;
        esp_now_protocol_send_f_state(fatigue, face_ok, 0, drv_conf, sys_status, id_evt,
                                      g_display_data.pred_minutes, pred_level);
        if (id_evt != IDENTIFY_EVENT_NONE) s_last_identify_event_sent = IDENTIFY_EVENT_NONE;
        s_last_fatigue    = fatigue;
        s_last_face_ok    = face_ok;
        s_last_drv_conf   = drv_conf;
        s_last_sys_status = sys_status;
        s_last_f_send_ms  = now;
    }
}

/* Send combined data to EG800K (every 4s or on state change) */
static void check_send_to_eg800k(void)
{
    uint32_t now = esp_timer_get_time() / 1000;
    int face_ok = s_face_recognized ? 1 : 0;
    int drv_conf = (s_current_driver_id > 0) ? s_current_driver_id : 0;
    int sys_status = s_dev_connected ? 1 : 2;
    int fatigue = (int)g_display_data.fatigue_level;
    float perclos = g_display_data.perclos;
    int alcohol = 0;  // MQ3 placeholder (alcohol sensor on VirtualGPS board)

    bool changed = (fatigue != s_last_fatigue) ||
                   (face_ok != s_last_face_ok) ||
                   (drv_conf != s_last_drv_conf) ||
                   (sys_status != s_last_sys_status);
    bool heartbeat = (now - s_last_eg800k_send_ms > 4000);

    if (changed || heartbeat) {
        int rd = (g_display_data.rest_recovery_pct >= 0) ? g_display_data.rest_total_minutes : 0;
        int rr = g_display_data.rest_recovery_pct;
        gps_uart_send_to_eg800k(fatigue, face_ok, drv_conf, sys_status, perclos, alcohol, rd, rr);
        s_last_fatigue = fatigue;
        s_last_face_ok = face_ok;
        s_last_drv_conf = drv_conf;
        s_last_sys_status = sys_status;
        s_last_eg800k_send_ms = now;
    }
}

/* Data collection mode — collect real eye ROI grayscale images for retraining */
static bool s_collect_mode = false;
static bool s_demo_enable = false;       /* set by serial 'demo' → fast prediction mode */
static int64_t s_last_yawn_us = 0;      /* timestamp of last yawn for boost decay */
static const char *s_collect_label = "unknown";

/* UVC callback */
static bool frame_callback(const uvc_host_frame_t *frame, void *user_ctx)
{
    assert(frame); assert(user_ctx);
    QueueHandle_t frame_q = *((QueueHandle_t *)user_ctx);
    BaseType_t result = xQueueSendToBack(frame_q, &frame, 0);
    if (pdPASS != result) {
        ESP_LOGW(TAG, "Frame queue full, dropping frame");
        return true;
    }
    return false;
}

static void stream_callback(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
        ESP_LOGE(TAG, "USB transfer error, err_no = %i", event->transfer_error.error);
        break;
    case UVC_HOST_DEVICE_DISCONNECTED:
        ESP_LOGI(TAG, "Camera disconnected");
        s_dev_connected = false;
        /* Don't close here — let main loop clean up to avoid race */
        break;
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
        ESP_LOGW(TAG, "Frame buffer overflow");
        break;
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        ESP_LOGW(TAG, "Frame buffer underflow");
        break;
    default: break;
    }
}

static void usb_lib_task(void *arg)
{
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB: All devices freed");
            if (s_usb_free_sem) xSemaphoreGive(s_usb_free_sem);
        }
    }
}

/* Poll stdin for commands (non-blocking getchar) */
static void poll_uart_commands(char *buf, int *pos, int maxlen)
{
    while (true) {
        int c = getchar();
        if (c < 0) break;

        if (c == '\n' || c == '\r') {
            if (*pos == 0) continue;
            buf[*pos] = 0;
            *pos = 0;

            if (strcmp(buf, "help") == 0) {
                printf("Commands: help / enroll / drivers / delete / collect / stop / open / closed / switch / weather <0-2> / rest [min] / rest stop / demo\n");
            } else if (strcmp(buf, "collect") == 0) {
                s_collect_mode = true;
                s_collect_label = "unknown";
                printf(">>> DATA COLLECTION MODE ON <<<\n");
                printf("  Type 'open'   → labels next frames as eye_open\n");
                printf("  Type 'closed' → labels next frames as eye_closed\n");
                printf("  Type 'stop'   → exit collection mode\n");
            } else if (strcmp(buf, "closed") == 0) {
                s_collect_label = "eye_closed";
                printf(">>> LABEL: eye_closed <<<\n");
            } else if (strcmp(buf, "stop") == 0) {
                s_collect_mode = false;
                printf(">>> DATA COLLECTION MODE OFF <<<\n");
            } else if (strcmp(buf, "open") == 0) {
                s_collect_label = "eye_open";
                printf(">>> LABEL: eye_open <<<\n");
            } else if (strncmp(buf, "enroll ", 7) == 0) {
                int id = atoi(buf + 7);
                if (id >= 1 && id <= FACE_MAX_DRIVERS) {
                    printf("Will enroll driver #%d on next face\n", id);
                    s_pending_enroll_id = id;
                } else {
                    printf("Invalid ID (1..%d)\n", FACE_MAX_DRIVERS);
                }
            } else if (strcmp(buf, "drivers") == 0) {
                int cnt = face_recognizer_get_enrolled_count();
                if (cnt == 0) {
                    printf("No drivers enrolled\n");
                } else {
                    char list[256];
                    face_recognizer_list_enrolled(list, sizeof(list));
                    printf("Drivers:\n%s", list);
                }
            } else if (strncmp(buf, "delete ", 7) == 0) {
                int id = atoi(buf + 7);
                printf(face_recognizer_delete(id) ? "Deleted\n" : "Failed\n");
            } else if (strcmp(buf, "mictest") == 0) {
                printf(">>> Running USB mic test...\n");
                mic_test_run();
            } else if (strcmp(buf, "switch") == 0) {
                s_detect_state = DetectState::IDENTIFYING;
                s_face_recognized = false;
                s_face_lost_frames = 0;
                s_force_reidentify = true;
                s_driving_start_ms = 0;
                s_identify_start_ms = esp_timer_get_time() / 1000;
                s_last_identify_event_sent = IDENTIFY_EVENT_START;
                printf(">>> State → IDENTIFYING, re-identify on next face <<<\n");
            } else if (strncmp(buf, "weather ", 8) == 0) {
                int risk = atoi(buf + 8);
                if (risk >= 0 && risk <= 2) {
                    g_weather_risk_raw = risk;
                    g_weather_pending = 1;  /* Signal main loop to sync */
                    const char *names[] = {"晴天", "雨雪", "雾霾"};
                    printf("Weather risk: %d (%s)\n", risk, names[risk]);
                    printf("  PERCLOS: mild=%.0f%% severe=%.0f%%\n",
                           risk == 0 ? 15.0f : (risk == 1 ? 12.0f : 10.0f),
                           risk == 0 ? 30.0f : (risk == 1 ? 25.0f : 20.0f));
                    printf("  Drive warn: %d min\n",
                           risk == 0 ? 210 : (risk == 1 ? 180 : 150));
                } else {
                    printf("Usage: weather 0(晴) / 1(雨雪) / 2(雾霾)\n");
                }
            } else if (strcmp(buf, "demo") == 0) {
                s_demo_enable = true;
                g_display_data.history_count = 0;  /* reset history for clean demo */
                g_display_data.pred_minutes = -1;
                g_display_data.pred_ready = false;  /* re-enter reference projection until data builds */
                printf("Demo mode ON: 5s snapshots, 10s predictions (for 10-min demo)\n");
            } else if (strcmp(buf, "rest stop") == 0) {
                if (!s_rest_active) {
                    printf("Not in rest mode\n");
                } else if (g_display_data.rest_mandatory) {
                    printf("REST LOCKED: mandatory rest (severe fatigue), cannot stop early\n");
                } else {
                    stop_rest(false);
                }
            } else if (strncmp(buf, "rest ", 5) == 0) {
                int min = atoi(buf + 5);
                if (min < 1) min = 1;
                if (min > 120) min = 120;
                start_rest(min);
            } else if (strcmp(buf, "rest") == 0) {
                start_rest(0);   /* auto-duration from fatigue level */
            } else {
                printf("Unknown: '%s' (type help)\n", buf);
            }
        } else if (c == 127 || c == '\b') {
            if (*pos > 0) (*pos)--;
        } else if (c >= 32 && c < 127) {
            if (*pos < maxlen - 1) buf[(*pos)++] = (char)c;
        }
    }
}

static bool process_frame(const uint8_t *jpeg_data, size_t jpeg_len,
                          int w, int h, uint8_t eye_gray96[96 * 96])
{
    jpeg_dec_config_t dec_cfg = DEFAULT_JPEG_DEC_CONFIG();
    dec_cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
    jpeg_dec_handle_t jpeg_dec = NULL;
    if (jpeg_dec_open(&dec_cfg, &jpeg_dec) != JPEG_ERR_OK) return false;

    jpeg_dec_io_t io;
    memset(&io, 0, sizeof(io));
    io.inbuf = const_cast<uint8_t *>(jpeg_data);
    io.inbuf_len = (int)jpeg_len;
    io.inbuf_remain = (int)jpeg_len;

    jpeg_dec_header_info_t header;
    if (jpeg_dec_parse_header(jpeg_dec, &io, &header) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "JPEG header parse failed (len=%u) — frame may be corrupt",
                 (unsigned)jpeg_len);
        jpeg_dec_close(jpeg_dec);
        return false;
    }
    int actual_w = (int)header.width;
    int actual_h = (int)header.height;

    int outbuf_len = 0;
    jpeg_dec_get_outbuf_len(jpeg_dec, &outbuf_len);
    uint8_t *rgb_buf = (uint8_t *)jpeg_calloc_align(outbuf_len, 16);
    if (!rgb_buf) { jpeg_dec_close(jpeg_dec); return false; }

    io.outbuf = rgb_buf;
    io.out_size = outbuf_len;
    if (jpeg_dec_process(jpeg_dec, &io) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "JPEG decode failed (len=%u, %dx%d) — frame corrupt",
                 (unsigned)jpeg_len, actual_w, actual_h);
        jpeg_free_align(rgb_buf);
        jpeg_dec_close(jpeg_dec);
        return false;
    }
    jpeg_dec_close(jpeg_dec);

    dl::image::img_t img;
    img.data = rgb_buf;
    img.width = (uint16_t)actual_w;
    img.height = (uint16_t)actual_h;
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565;

    std::list<dl::detect::result_t> &results = s_detector->run(img);

    bool ret = false;
    if (!results.empty()) {
        const auto &face = results.front();
        int fw = face.box[2] - face.box[0];
        int fh = face.box[3] - face.box[1];

        /* Debug: log detection result even if not meeting threshold */
        static uint32_t debug_frame_count = 0;
        if (++debug_frame_count % 30 == 0) {  /* Every 30 frames (~6s) */
            ESP_LOGI(TAG, "Face candidate: [%d,%d,%d,%d] size=%dx%d score=%.3f (need: >=60x60, score>=0.25)",
                     face.box[0], face.box[1], face.box[2], face.box[3], fw, fh, face.score);
        }

        /* Require: face >= 60x60px AND score >= 0.25 (25% confidence) */
        if (fw >= 60 && fh >= 60 && face.score >= 0.25f) {
            roi_rect_t eye_roi = estimate_eye_roi(face);
            rgb565_frame_t fb;
            fb.buf = rgb_buf;
            fb.width = actual_w;
            fb.height = actual_h;
            if (eye_gray96) ret = crop_to_gray96(&fb, eye_roi, eye_gray96);
            else ret = true;

            if (ret) {
                ESP_LOGI(TAG, "Face:[%d,%d,%d,%d](%dx%d) score=%.3f EyeROI:[%d,%d,%dx%d]",
                         face.box[0], face.box[1], face.box[2], face.box[3], fw, fh, face.score,
                         eye_roi.x, eye_roi.y, eye_roi.w, eye_roi.h);

                /* Update display data - face info */
                g_display_data.face_detected = true;
                g_display_data.face_w = fw;
                g_display_data.face_h = fh;

                /* ── Mouth ROI: yawn detection via dark-pixel ratio ── */
                {
                    roi_rect_t mouth_roi = estimate_mouth_roi(face);
                    /* Small on-stack buffer for mouth grayscale (32x48 is enough for dark ratio) */
                    constexpr int MW = 48, MH = 32;
                    uint8_t mouth_gray[MW * MH];
                    if (crop_mouth_to_gray(&fb, mouth_roi, mouth_gray, MW, MH)) {
                        float dr = compute_dark_ratio(mouth_gray, MW, MH);
                        g_display_data.mouth_dark_ratio = dr;

                        /* ── Time-based yawn detection (frame-rate independent) ── */
                        static int64_t s_yawn_start_us = 0;  /* when mouth first opened */
                        static bool    s_yawn_fired   = false;
                        static int64_t s_yawn_end_us   = 0;  /* when mouth last closed (for cooldown) */

                        int64_t now_us = esp_timer_get_time();

                        if (dr > MOUTH_DARK_RATIO_YAWN) {
                            if (s_yawn_start_us == 0) {
                                s_yawn_start_us = now_us;   /* mouth just opened */
                            }
                            int32_t elapsed_ms = (int32_t)((now_us - s_yawn_start_us) / 1000);
                            if (elapsed_ms >= MOUTH_YAWN_DURATION_MS && !s_yawn_fired) {
                                s_yawn_fired = true;
                                s_yawn_end_us = 0;
                                g_display_data.yawn_count++;
                                g_display_data.yawn_active = true;
                                /* PERCLOS boost: +5% per yawn, max +15% */
                                g_display_data.yawn_boost += 5;
                                if (g_display_data.yawn_boost > 15) g_display_data.yawn_boost = 15;
                                s_last_yawn_us = now_us;
                                ESP_LOGW(TAG, "╔══════════════════════════════╗");
                                ESP_LOGW(TAG, "║  YAWN DETECTED!  #%-3d       ║", g_display_data.yawn_count);
                                ESP_LOGW(TAG, "║  dark_ratio=%.2f  dur=%dms   ║", dr, elapsed_ms);
                                ESP_LOGW(TAG, "║  PERCLOS boost +5%% = %d%%     ║", g_display_data.yawn_boost);
                                ESP_LOGW(TAG, "╚══════════════════════════════╝");
                            }
                        } else {
                            /* Mouth closed: record time, reset counter */
                            if (s_yawn_start_us != 0) {
                                s_yawn_end_us = now_us;
                                s_yawn_start_us = 0;
                            }
                            /* Cooldown: mouth must stay closed for 1s before next yawn */
                            if (s_yawn_fired && s_yawn_end_us > 0 &&
                                (now_us - s_yawn_end_us) > 1000000) {
                                s_yawn_fired = false;
                                g_display_data.yawn_active = false;
                                ESP_LOGI(TAG, "Yawn cooldown done (total yawns: %d)", g_display_data.yawn_count);
                            }
                        }

                        /* Update yawn_frames for display (elapsed time / 100ms for UI) */
                        if (s_yawn_start_us > 0) {
                            g_display_data.yawn_frames = (int)((now_us - s_yawn_start_us) / 100);
                        } else {
                            g_display_data.yawn_frames = 0;
                        }
                    }
                }
            }

            /* DATA COLLECTION MODE: dump eye ROI grayscale every frame, no classifier needed */
            if (ret && s_collect_mode && eye_gray96) {
                printf("==GRAY_START label=%s==\n", s_collect_label);
                for (int y = 0; y < 96; y++) {
                    for (int x = 0; x < 96; x++) printf("%02x", eye_gray96[y * 96 + x]);
                    printf("\n");
                }
                printf("==GRAY_END==\n");
                /* Don't return yet — still do identity check / enroll below */
            }

            /* Face (re-)identification trigger:
             * - First face ever detected
             * - Face re-appears after being lost >30s (50 frames @ ~1.5fps)
             * - Manual switch command via serial */
            bool do_identify = false;
            if (ret && !s_collect_mode) {
                if (!s_face_recognized) {
                    do_identify = true;
                } else if (s_face_lost_frames >= 50) {
                    do_identify = true;
                }
            }

            if (do_identify) {
                s_face_lost_frames = 0;
                int box[4] = { face.box[0], face.box[1], face.box[2], face.box[3] };
                ESP_LOGI(TAG, ">>> Face identity check (state=%s, driver #%d active)... <<<",
                         s_detect_state == DetectState::IDENTIFYING ? "IDENTIFYING" : "MONITORING",
                         s_current_driver_id);

                /* ── Use Local MobileFaceNet (cloud disabled) ── */
                float feature[FACE_FEAT_DIM];
                if (face_recognizer_extract(rgb_buf, actual_w, actual_h, box, feature)) {
                    face_recognition_result_t recog =
                        face_recognizer_identify(feature, FACE_DEFAULT_THRESHOLD);
                    if (recog.is_known) {
                        /* When re-identifying after VirtualGPS driver switch,
                         * verify the face matches the expected driver. */
                        bool vgps_mismatch = false;
                        if (s_detect_state == DetectState::IDENTIFYING &&
                            g_display_data.gps_connected &&
                            g_display_data.gps_driver_id > 0 &&
                            recog.driver_id != g_display_data.gps_driver_id) {
                            ESP_LOGW(TAG, ">>> MISMATCH: VGPS expects driver #%d "
                                     "but face is driver #%d (sim=%.1f%%) — retrying <<<",
                                     g_display_data.gps_driver_id, recog.driver_id,
                                     recog.similarity * 100.0f);
                            vgps_mismatch = true;
                            s_face_recognized = false;
                            s_last_identify_event_sent = IDENTIFY_EVENT_FAIL;
                        }

                        if (!vgps_mismatch) {
                            s_face_recognized = true;
                            /* Driver switch detection */
                            if (s_current_driver_id > 0 && recog.driver_id != s_current_driver_id) {
                                ESP_LOGI(TAG, ">>> [LOCAL] DRIVER SWITCH: #%d → #%d (sim=%.1f%%) <<<",
                                         s_current_driver_id, recog.driver_id,
                                         recog.similarity * 100.0f);
                                s_driving_start_ms = 0;
                            }
                            s_current_driver_id = recog.driver_id;
                            /* Transition IDENTIFYING → MONITORING */
                            if (s_detect_state == DetectState::IDENTIFYING) {
                                s_detect_state = DetectState::MONITORING;
                                s_driving_start_ms = 0;  /* Trigger session reset on next face */
                                s_last_identify_event_sent = IDENTIFY_EVENT_SUCCESS;
                                ESP_LOGI(TAG, ">>> State: IDENTIFYING → MONITORING <<<");
                            }
                            ESP_LOGI(TAG, ">>> [LOCAL] DRIVER #%d confirmed | similarity=%.1f%% <<<",
                                     recog.driver_id, recog.similarity * 100.0f);
                            g_display_data.driver_id = recog.driver_id;
                            g_display_data.driver_similarity = recog.similarity;
                        }
                    } else {
                        ESP_LOGW(TAG, ">>> [LOCAL] STRANGER | best similarity=%.1f%% <<<",
                                 recog.similarity * 100.0f);
                        if (face_recognizer_get_enrolled_count() == 0) {
                            ESP_LOGI(TAG, ">>> Auto-enrolling this face as DRIVER #1 <<<");
                            if (face_recognizer_enroll(1, feature, NULL)) {
                                s_current_driver_id = 1;
                                s_face_recognized = true;
                                g_display_data.driver_id = 1;
                                s_detect_state = DetectState::MONITORING;
                                s_driving_start_ms = 0;
                                s_last_identify_event_sent = IDENTIFY_EVENT_SUCCESS;
                                ESP_LOGI(TAG, ">>> DRIVER #1 enrolled OK, state → MONITORING <<<");
                                face_recognition_result_t check =
                                    face_recognizer_identify(feature, 0.01f);
                                if (check.is_known)
                                    ESP_LOGI(TAG, ">>> SELF-CHECK PASSED: match=%.1f%% <<<",
                                             check.similarity * 100.0f);
                            }
                        } else {
                            /* Stay in IDENTIFYING, retry on next face */
                            s_face_recognized = false;
                            if (s_detect_state == DetectState::IDENTIFYING) {
                                s_last_identify_event_sent = IDENTIFY_EVENT_FAIL;
                            }
                        }
                    }
                } else {
                    s_face_recognized = false;  /* Extract failed, retry */
                }
            }

            /* Pending enrollment (from serial command "enroll <ID>") */
            if (ret && !s_collect_mode && s_pending_enroll_id > 0) {
                int eid = s_pending_enroll_id;
                s_pending_enroll_id = 0;
                int box[4] = { face.box[0], face.box[1], face.box[2], face.box[3] };
                ESP_LOGI(TAG, ">>> Enrolling driver #%d... <<<", eid);
                float feature[FACE_FEAT_DIM];
                if (face_recognizer_extract(rgb_buf, actual_w, actual_h, box, feature)) {
                    if (face_recognizer_enroll(eid, feature, NULL)) {
                        ESP_LOGI(TAG, ">>> DRIVER #%d enrolled OK <<<", eid);
                        /* Verify: query with same feature, should match self */
                        face_recognition_result_t self_check =
                            face_recognizer_identify(feature, FACE_DEFAULT_THRESHOLD);
                        if (self_check.is_known) {
                            ESP_LOGI(TAG, ">>> SELF-CHECK: driver #%d sim=%.1f%% (CORRECT) <<<",
                                     self_check.driver_id, self_check.similarity * 100.0f);
                        } else {
                            ESP_LOGW(TAG, ">>> SELF-CHECK FAILED: sim=%.1f%% (NVS may be corrupt) <<<",
                                     self_check.similarity * 100.0f);
                        }
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "Face too small: %dx%d", fw, fh);
            ret = false;
        }
    }
    jpeg_free_align(rgb_buf);
    return ret;
}

/* ── 疲劳预测: 线性回归 on PERCLOS history, 算还有几分钟到下一阈值.
 * 独立于 face_found (demo ramp 无脸时也要算, 否则 Demo 模式没预测语音) ── */
static void run_fatigue_prediction(void)
{
    static uint32_t s_last_pred_ms = 0;
    uint32_t now_ms = esp_timer_get_time() / 1000;
    /* 2 个采样点即可出预测(10s 采样×2 + 10s 周期 → 真实模式约 30s 出预测) */
    int min_samples = 2;
    int pred_ms     = 10000;   /* 真实模式原 30s 周期, 提速到 10s */

    if (g_display_data.history_count < min_samples) {
        g_display_data.pred_ready = false;  /* 数据不够 → UI 画参考虚线 */
        return;
    }
    if (now_ms - s_last_pred_ms < (uint32_t)pred_ms) return;
    s_last_pred_ms = now_ms;

    int n = g_display_data.history_count;
    if (n > 20) n = 20;
    float sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
    for (int i = 0; i < n; i++) {
        float x = (float)i;
        float y = g_display_data.perclos_history[i % 20];
        sum_x += x; sum_y += y; sum_xy += x * y; sum_x2 += x * x;
    }
    float denom = n * sum_x2 - sum_x * sum_x;
    if (denom > 0.001f) {
        float slope_30s = (n * sum_xy - sum_x * sum_y) / denom;
        g_display_data.perclos_slope = slope_30s * 2.0f;  /* per minute */
    } else {
        g_display_data.perclos_slope = 0;
    }
    g_display_data.pred_ready = true;  /* 回归已运行 → UI 切换为真实预测/平稳判定 */

    float cur = g_display_data.perclos;   /* demo 时 g_display_data.perclos = ramp 值 */
    if (g_display_data.perclos_slope > 0.05f) {
        float thresh = (g_display_data.fatigue_level >= 1) ? 30.0f : 15.0f;
        float gap = thresh - cur;
        if (gap > 0) g_display_data.pred_minutes = (int)(gap / g_display_data.perclos_slope);
        else         g_display_data.pred_minutes = 0;   /* 已到阈值 */
    } else {
        g_display_data.pred_minutes = -1;  /* 上升不明显 */
    }
    ESP_LOGI(TAG, "Predict: slope=%.2f/min pred=%dmin (n=%d perclos=%.1f)",
             g_display_data.perclos_slope, g_display_data.pred_minutes, n, cur);
}

static void frame_handling_task(void *arg)
{
    const uvc_host_stream_config_t *cfg = (const uvc_host_stream_config_t *)arg;
    QueueHandle_t frame_q = *((QueueHandle_t *)(cfg->user_ctx));
    uint32_t frame_count = 0;

    uint8_t *eye_gray96 = (uint8_t *)heap_caps_malloc(96 * 96, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!eye_gray96) eye_gray96 = new uint8_t[96 * 96]();

    PerclosCalculator<30> s_perclos;
    FatigueLogger s_fatigue_log;
    uint32_t total_frames = 0, face_frames = 0, closed_frames = 0;

    /* Fatigue level change → (TTS/display 待实现) */

    char cmd_buf[64];
    int cmd_pos = 0;
    memset(cmd_buf, 0, sizeof(cmd_buf));
    ESP_LOGI(TAG, "Commands: help / enroll <ID> / drivers / delete <ID>");

    while (true) {
        poll_uart_commands(cmd_buf, &cmd_pos, sizeof(cmd_buf));
        esp_now_protocol_poll();  /* Process ESP-NOW data from VirtualGPS */
        gps_uart_poll_eg800k();   /* Receive GPS from EG800K */

        /* Sync weather risk to perclos calculator (from serial cmd or auto G-prefix) */
        if (g_weather_pending) {
            s_weather_risk = (WeatherRisk)g_weather_risk_raw;
            s_perclos.set_weather_risk(s_weather_risk);
            g_display_data.weather_risk = (uint8_t)s_weather_risk;
            g_display_data.drive_warn_min = (uint8_t)(s_perclos.get_drive_warning_minutes() / 10);
            g_weather_pending = 0;
        }

        /* Rest request from VirtualGPS BUTTON_3 → auto-duration from fatigue level */
        if (g_rest_request_pending) {
            g_rest_request_pending = 0;
            start_rest(0);
        }

        check_send_f_prefix();    /* Send F-prefix to VirtualGPS */
        check_send_to_eg800k();   /* Send combined data to EG800K */
        /* (display flags synced in inner loop after camera opens) */

        uvc_host_stream_hdl_t uvc_stream = NULL;
        /* Wait for USB host to finish freeing old devices before attempting open */
        if (xSemaphoreTake(s_usb_free_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
            ESP_LOGW(TAG, "USB free timeout, attempting open anyway");
        }
        ESP_LOGI(TAG, "Waiting for UVC camera...");
        esp_err_t err = uvc_host_stream_open(cfg, pdMS_TO_TICKS(5000), &uvc_stream);
        if (ESP_OK != err) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
        s_dev_connected = true;
        ESP_LOGI(TAG, "UVC camera OPENED!");

        /* Init LVGL display after USB has its memory (one-time) */
        {
            static bool lvgl_done = false;
            if (!lvgl_done) {
                ESP_LOGI(TAG, "Initializing LVGL display...");
                lvgl_ui_init_display();
                lvgl_done = true;
            }
        }

        err = uvc_host_stream_start(uvc_stream);
        if (ESP_OK != err) { uvc_host_stream_close(uvc_stream); vTaskDelay(pdMS_TO_TICKS(2000)); continue; }

        s_perclos.reset();
        s_perclos.set_weather_risk(s_weather_risk);
        total_frames = 0; face_frames = 0; closed_frames = 0;
        /* Send IDENTIFY_EVENT_START on camera (re)open */
        s_identify_start_ms = esp_timer_get_time() / 1000;
        if (s_detect_state == DetectState::IDENTIFYING) {
            s_last_identify_event_sent = IDENTIFY_EVENT_START;
        }
        const int PROCESS_EVERY_N_FRAMES = 10;
        int skip_counter = 0;

        while (s_dev_connected) {
            poll_uart_commands(cmd_buf, &cmd_pos, sizeof(cmd_buf));
            esp_now_protocol_poll();  /* Process ESP-NOW data from VirtualGPS */
            gps_uart_poll_eg800k();   /* EG800K GPS data */

            /* Rest request from VirtualGPS BUTTON_3 → start_rest (also checked in outer loop) */
            if (g_rest_request_pending) {
                g_rest_request_pending = 0;
                if (!s_rest_active) start_rest(0);  /* 0=auto from fatigue level */
            }
            /* Rest stop request from VirtualGPS BUTTON_3 (toggle) */
            if (g_rest_stop_pending) {
                g_rest_stop_pending = 0;
                if (s_rest_active) {
                    if (g_display_data.rest_mandatory) {
                        ESP_LOGW(TAG, "REST STOP DENIED: mandatory rest (severe fatigue)");
                    } else {
                        stop_rest(false);
                    }
                }
            }

            /* ── Rest mode countdown & auto-exit ── */
            if (s_rest_active) {
                int elapsed = (int)((esp_timer_get_time() / 1000 - s_rest_start_ms) / 1000);
                g_display_data.rest_elapsed_seconds = elapsed;
                int total_sec = s_rest_total_min * 60;
                if (elapsed >= total_sec) {
                    /* Rest timer done — check if recovery is sufficient */
                    /* Note: PERCLOS will be measured after rest exits (post-rest warm-up).
                     * For now, exit rest and let the post-rest block handle recovery check.
                     * If recovery < 40% after warm-up, extend_count is used to limit re-extension. */
                    s_rest_active = false;
                    g_display_data.rest_active = false;
                    /* Post-rest processing happens below (after this if block) */
                    ESP_LOGI(TAG, "<<< REST TIMER DONE: %d min elapsed | recovery check pending <<<",
                             s_rest_total_min);
                } else {
                    /* During rest: no fatigue detection. Drain frames so the
                     * UVC driver doesn't stall on full frame buffers. */
                    uvc_host_frame_t *f;
                    while (xQueueReceive(frame_q, &f, 0) == pdPASS)
                        uvc_host_frame_return(uvc_stream, f);
                    /* Keep heartbeats so VirtualGPS/EG800K stay connected */
                    check_send_f_prefix();
                    check_send_to_eg800k();
                    vTaskDelay(pdMS_TO_TICKS(200));
                    continue;
                }
            }

            /* ── Post-rest: trigger face re-verification (only on auto-complete, not manual stop) ── */
            {
                static bool s_post_rest_done = false;
                if (!s_rest_active && g_display_data.rest_recovery_pct == -1 &&
                    g_display_data.rest_snap_perclos > 0 && !s_post_rest_done &&
                    g_display_data.rest_face_ok == false) {  /* manual stop sets face_ok=true to skip */
                    s_post_rest_done = true;
                    /* Save rest result — recovery computed from rest_snap vs current perclos */
                    float snap = g_display_data.rest_snap_perclos;
                    float cur  = g_display_data.perclos;
                    int recov = -1;
                    if (snap > 0.5f && cur >= 0) {
                        recov = (int)((snap - cur) / snap * 100.0f);
                        if (recov < 0) recov = 0;
                        if (recov > 100) recov = 100;
                    }
                    g_display_data.rest_recovery_pct = recov;
                    s_last_identify_event_sent = IDENTIFY_EVENT_REST_DONE;

                    ESP_LOGI(TAG, "╔══════════════════════════════╗");
                    ESP_LOGI(TAG, "║  REST DONE: %d min           ║", s_rest_total_min);
                    ESP_LOGI(TAG, "║  PERCLOS: %.1f → %.1f%%       ║", snap, cur);
                    ESP_LOGI(TAG, "║  recovery=%d%%                 ║", recov);
                    ESP_LOGI(TAG, "╚══════════════════════════════╝");

                    /* Show result on rest page for 3s before face re-verify */
                    s_rest_result_show_ms = esp_timer_get_time() / 1000;
                }
                if (s_rest_active) s_post_rest_done = false;  /* reset for next rest */
            }

            /* ── Show recovery result for 3s before face re-verify ── */
            if (s_rest_result_show_ms > 0) {
                uint32_t now_ms = esp_timer_get_time() / 1000;
                if (now_ms - s_rest_result_show_ms < 3000) {
                    g_display_data.rest_active = true;   /* keep rest page visible */
                    check_send_f_prefix();
                    check_send_to_eg800k();
                    vTaskDelay(pdMS_TO_TICKS(200));
                    continue;
                }
                s_rest_result_show_ms = 0;
                g_display_data.rest_active = false;
                s_face_recognized = false;
                s_detect_state = DetectState::IDENTIFYING;
                s_identify_start_ms = esp_timer_get_time() / 1000;
                s_perclos.reset();
                g_display_data.identifying = true;
                ESP_LOGI(TAG, ">>> Post-rest face re-verify started <<<");
            }

            /* Detect VirtualGPS driver_id change (user pressed BUTTON_5) */
            {
                static int s_last_vgps_driver_id = 0;
                if (s_detect_state == DetectState::MONITORING &&
                    g_display_data.gps_connected &&
                    g_display_data.gps_driver_id != s_last_vgps_driver_id &&
                    s_last_vgps_driver_id != 0) {
                    ESP_LOGI(TAG, ">>> VirtualGPS driver switch, state → IDENTIFYING <<<");
                    s_detect_state = DetectState::IDENTIFYING;
                    s_face_recognized = false;
                    s_face_lost_frames = 0;
                    s_driving_start_ms = 0;
                    s_identify_start_ms = esp_timer_get_time() / 1000;
                    s_last_identify_event_sent = IDENTIFY_EVENT_START;
                }
                s_last_vgps_driver_id = g_display_data.gps_driver_id;
            }

            /* Sync state to LVGL display */
            g_display_data.camera_ready = s_dev_connected;
            g_display_data.identifying = (s_detect_state == DetectState::IDENTIFYING);

            /* Sync SNTP time (every 1s) */
            {
                static uint32_t s_last_ts = 0;
                uint32_t ns = esp_timer_get_time() / 1000000;
                if (ns - s_last_ts > 1) {
                    time_t now; struct tm ti;
                    time(&now); localtime_r(&now, &ti);
                    if (ti.tm_year >= 120) {
                        g_display_data.time_hour = ti.tm_hour;
                        g_display_data.time_min = ti.tm_min;
                        g_display_data.time_sec = ti.tm_sec;
                        g_display_data.time_year = ti.tm_year + 1900;
                        g_display_data.time_month = ti.tm_mon + 1;
                        g_display_data.time_day = ti.tm_mday;
                        g_display_data.time_weekday = ti.tm_wday;
                        g_display_data.time_valid = true;
                    }
                    s_last_ts = ns;
                }
            }

            uvc_host_frame_t *frame;
            if (xQueueReceive(frame_q, &frame, pdMS_TO_TICKS(5000)) != pdPASS) {
                ESP_LOGW(TAG, "Frame timeout"); break;
            }
            frame_count++;
            if (!s_collect_mode) {
                if (++skip_counter < PROCESS_EVERY_N_FRAMES) { uvc_host_frame_return(uvc_stream, frame); continue; }
                skip_counter = 0;
            }

            { /* Drain stale frames */
                uvc_host_frame_t *stale;
                while (xQueueReceive(frame_q, &stale, 0) == pdPASS) uvc_host_frame_return(uvc_stream, stale);
            }

            uvc_host_stream_format_t fmt;
            uvc_host_stream_format_get(uvc_stream, &fmt);

            int64_t t0 = esp_timer_get_time();
            bool face_found = process_frame(frame->data, frame->data_len, fmt.h_res, fmt.v_res, eye_gray96);
            int64_t elapsed = (esp_timer_get_time() - t0) / 1000;
            uvc_host_frame_return(uvc_stream, frame);
            /* Drain frames that piled up during slow processing (eye clf ~1.2s / face recog ~12.6s) */
            {
                uvc_host_frame_t *drain;
                while (xQueueReceive(frame_q, &drain, 0) == pdPASS)
                    uvc_host_frame_return(uvc_stream, drain);
            }

            /* ── Demo ramp: VirtualGPS Rainmaker "Demo Mode" button ──
             * 每 10 秒 PERCLOS +5，喂给预测历史，自动演示疲劳预测曲线。
             * 独立于真实人脸检测，电池供电无串口也能触发。 */
            {
                static float    s_demo_ramp_val     = 5.0f;
                static uint32_t s_demo_ramp_last_ms = 0;

                if (g_demo_cmd_pending >= 0) {
                    if (g_demo_cmd_pending == 1 && !s_demo_ramp_active) {
                        s_demo_ramp_active = true;
                        s_demo_ramp_val = 5.0f;
                        s_demo_ramp_last_ms = esp_timer_get_time() / 1000;
                        g_display_data.history_count = 0;
                        g_display_data.pred_minutes = -1;
                        g_display_data.pred_ready = false;  /* re-enter reference projection until ramp data builds */
                        s_demo_enable = true;  /* 开启快速采样/预测 (5s/10s) */
                        ESP_LOGW(TAG, "╔══════════════════════════════╗");
                        ESP_LOGW(TAG, "║  DEMO RAMP STARTED           ║");
                        ESP_LOGW(TAG, "║  PERCLOS +5%% every 10s       ║");
                        ESP_LOGW(TAG, "╚══════════════════════════════╝");
                    } else if (g_demo_cmd_pending == 0 && s_demo_ramp_active) {
                        s_demo_ramp_active = false;
                        ESP_LOGW(TAG, ">>> DEMO RAMP STOPPED <<<");
                    }
                    g_demo_cmd_pending = -1;  /* consume */
                }

                if (s_demo_ramp_active) {
                    uint32_t now_ms = esp_timer_get_time() / 1000;
                    if (now_ms - s_demo_ramp_last_ms >= 10000) {
                        s_demo_ramp_last_ms = now_ms;
                        /* 随机 +0.8~2.3%/10s 慢速爬升: 太快预测≈0分钟(无意义),
                         * 慢速才让"预计X分钟后进入轻度/严重疲劳"有真实感 */
                        float inc = 0.8f + (float)(esp_random() % 150) / 100.0f;
                        s_demo_ramp_val += inc;
                        if (s_demo_ramp_val > 50.0f) s_demo_ramp_val = 5.0f;  /* 封顶循环 */
                        /* 覆盖显示值 + 疲劳等级 */
                        g_display_data.perclos = s_demo_ramp_val;
                        g_display_data.fatigue_level =
                            (s_demo_ramp_val > 30.0f) ? 2 : (s_demo_ramp_val > 15.0f) ? 1 : 0;
                        /* 喂预测历史 */
                        int hidx = g_display_data.history_count % 20;
                        g_display_data.perclos_history[hidx] = s_demo_ramp_val;
                        g_display_data.history_count++;
                        if (g_display_data.history_count > 20)
                            g_display_data.history_count = 20;
                        ESP_LOGI(TAG, "DEMO RAMP: perclos=%.1f%% level=%d",
                                 s_demo_ramp_val, g_display_data.fatigue_level);
                        run_fatigue_prediction();   /* demo 无脸也要算预测(才有预测语音) */
                    }
                }
            }

            if (face_found) {
                face_frames++; total_frames++;
                s_face_lost_frames = 0;  /* reset lost counter */

                /* ── Driver switch: reset session timer + PERCLOS ── */
                if (s_driving_start_ms == 0 && s_current_driver_id > 0) {
                    s_driving_start_ms = esp_timer_get_time() / 1000;
                    s_perclos.reset();
                    total_frames = 0; face_frames = 0; closed_frames = 0;
                    g_display_data.driving_minutes = 0;
                    g_display_data.total_frames = 0;
                    g_display_data.face_frames = 0;
                    g_display_data.closed_frames = 0;
                    g_display_data.valid_count = 0;
                    g_display_data.is_warm = false;
                    ESP_LOGI(TAG, ">>> Session reset for DRIVER #%d <<<",
                             s_current_driver_id);
                }

                /* COLLECT MODE: skip classifier/PERCLOS entirely, every frame */
                if (s_collect_mode) {
                    continue;
                }

                /* IDENTIFYING state: skip eye classifier + PERCLOS, wait for identity */
                if (s_detect_state == DetectState::IDENTIFYING) {
                    continue;
                }

                /* Stopped (speed <= 10): pause fatigue detection */
                if (g_display_data.gps_connected && g_display_data.gps_speed <= 10.0f) {
                    continue;
                }

                float eye_open = 0.0f, eye_closed = 0.0f;
                int64_t t_clf = esp_timer_get_time();

                /* Debug dump */
                static int dump_cnt = 0;
                if (++dump_cnt % 20 == 0) {
                    printf("==GRAY_START==\n");
                    for (int y = 0; y < 96; y++) {
                        for (int x = 0; x < 96; x++) printf("%02x", eye_gray96[y * 96 + x]);
                        printf("\n");
                    }
                    printf("==GRAY_END==\n");
                }

                bool clf_ok = eye_classifier_predict(eye_gray96, &eye_open, &eye_closed);
                int64_t clf_elapsed = (esp_timer_get_time() - t_clf) / 1000;

                if (clf_ok) {
                    bool is_closed = (eye_closed > eye_open);
                    if (is_closed) closed_frames++;
                    s_perclos.add_sample(is_closed);
                    float base_perclos = s_perclos.get_perclos();

                    /* ── Yawn boost decay: -1% every 60s without yawning ── */
                    {
                        int64_t now_us = esp_timer_get_time();
                        if (g_display_data.yawn_boost > 0 && s_last_yawn_us > 0) {
                            int64_t elapsed_s = (now_us - s_last_yawn_us) / 1000000;
                            /* One tick every 60s; never go below 0 */
                            int decay_ticks = (int)(elapsed_s / 60);
                            int new_boost = g_display_data.yawn_boost - decay_ticks;
                            if (new_boost < 0) new_boost = 0;
                            if (new_boost != g_display_data.yawn_boost) {
                                g_display_data.yawn_boost = new_boost;
                                if (new_boost == 0) {
                                    ESP_LOGI(TAG, "Yawn boost fully decayed");
                                }
                            }
                        }
                    }

                    float perclos = base_perclos + (float)g_display_data.yawn_boost;
                    if (perclos > 100.0f) perclos = 100.0f;

                    FatigueLevel lv = s_perclos.get_fatigue_level_for(perclos);
                    s_fatigue_log.update(lv, perclos, s_perclos.get_valid_count());

                    /* Update display data - PERCLOS + driving time.
                     * Rainmaker mock perclos (>=0) 覆盖显示值(不改真实检测/疲劳等级计算).
                     * Demo ramp 激活时: ramp 自己写 perclos/fatigue, 这里跳过覆盖 */
                    if (!s_demo_ramp_active) {
                        int mock_pcl = gps_uart_get_mock_perclos();
                        g_display_data.perclos = (mock_pcl >= 0) ? (float)mock_pcl : perclos;
                        g_display_data.fatigue_level = (uint8_t)lv;
                    }
                    g_display_data.total_frames = total_frames;
                    g_display_data.face_frames = face_frames;
                    g_display_data.closed_frames = closed_frames;
                    g_display_data.valid_count = s_perclos.get_valid_count();
                    g_display_data.is_warm = s_perclos.is_warm();
                    if (s_driving_start_ms > 0) {
                        uint32_t now_ms = esp_timer_get_time() / 1000;
                        g_display_data.driving_minutes =
                            (now_ms - s_driving_start_ms) / 60000;
                    }
                    /* ── Fatigue prediction: snapshot history, run shared regression ── */
                    {
                        static uint32_t s_last_snap_ms = 0;
                        static bool     s_demo_mode = false;
                        uint32_t now_ms = esp_timer_get_time() / 1000;

                        /* Demo mode: 5s snap (serial 'demo' command) */
                        if (s_demo_enable) { s_demo_mode = true; s_demo_enable = false; }
                        int snap_ms = s_demo_mode ? 5000 : 10000;   /* 真实模式 10s(原30s), 让预测更快 */

                        /* Snapshot PERCLOS (monitoring + 至少1/3窗口样本, 不再等窗口完全变热) */
                        if (now_ms - s_last_snap_ms > (uint32_t)snap_ms &&
                            s_detect_state == DetectState::MONITORING &&
                            s_perclos.get_valid_count() >= 10) {
                            s_last_snap_ms = now_ms;
                            int idx = g_display_data.history_count % 20;
                            g_display_data.perclos_history[idx] = perclos;
                            g_display_data.history_count++;
                            if (g_display_data.history_count > 20)
                                g_display_data.history_count = 20;
                        }

                        run_fatigue_prediction();   /* 回归 + 时间门控(demo/真实通用) */
                    }

                    check_send_f_prefix();  /* Send F-prefix on change */
                    check_send_to_eg800k();  /* Send combined data to EG800K */

                    if (frame_count % 15 == 0) {
                        ESP_LOGI(TAG, "[%4lu] Face OK | eye_open=%.3f eye_closed=%.3f %s | "
                                 "Mouth(dark)=%.2f yawn=%d/%d | "
                                 "PERCLOS=%.1f%% (%s) | tot=%lu face=%lu closed=%lu(%.1f%%) | "
                                 "detect=%lldms clf=%lldms pred=%dmin",
                                 frame_count, eye_open, eye_closed, is_closed ? ".CLOSED" : ".OPEN",
                                 g_display_data.mouth_dark_ratio,
                                 g_display_data.yawn_frames, MOUTH_YAWN_DURATION_MS,
                                 perclos, fatigue_level_str(lv),
                                 g_display_data.pred_minutes,
                                 total_frames, face_frames, closed_frames,
                                 face_frames > 0 ? (100.0f * closed_frames / face_frames) : 0.0f,
                                 elapsed, clf_elapsed);
                    }
                }
            } else {
                total_frames++;
                s_face_lost_frames++;
                /* After ~3 min of no face, allow fresh re-identification */
                if (s_face_lost_frames > 300) s_face_recognized = false;

                /* IDENTIFYING timeout: no face for 15s → notify VirtualGPS */
                if (s_detect_state == DetectState::IDENTIFYING) {
                    uint32_t now_ms = esp_timer_get_time() / 1000;
                    if (s_identify_start_ms > 0 &&
                        now_ms - s_identify_start_ms > IDENTIFY_TIMEOUT_MS) {
                        ESP_LOGW(TAG, ">>> IDENTIFYING timeout (%.1fs no face) <<<",
                                 (now_ms - s_identify_start_ms) / 1000.0f);
                        s_last_identify_event_sent = IDENTIFY_EVENT_FAIL;
                        s_identify_start_ms = now_ms;  /* Reset timer for next cycle */
                    }
                }

                /* Send F-prefix + EG800K data even without face (for timeout events) */
                check_send_f_prefix();
                check_send_to_eg800k();

                /* Update display - no face detected */
                g_display_data.face_detected = false;
                g_display_data.total_frames = total_frames;
            }
        }
        /* Clean up — handle both soft timeout and USB disconnect */
        if (s_dev_connected) uvc_host_stream_stop(uvc_stream);
        uvc_host_stream_close(uvc_stream);
        /* Drain stale frames from queue (pointers invalid after close) */
        {
            uvc_host_frame_t *drain;
            while (xQueueReceive(frame_q, &drain, 0) == pdPASS) { /* discard */ }
        }
        /* Wait for USB host to release endpoints before reopening */
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI(TAG, "Device disconnected, reopening...");
    }
    heap_caps_free(eye_gray96);
}

static const uvc_host_stream_config_t s_stream_config = {
    .event_cb = stream_callback,
    .frame_cb = frame_callback,
    .user_ctx = &s_frame_queue,
    .usb = { UVC_HOST_ANY_DEV_ADDR, UVC_HOST_ANY_VID, UVC_HOST_ANY_PID, 0 },
    .vs_format = { 1280, 720, 0, UVC_VS_FORMAT_MJPEG },
    /* { frame_buffers=8, frame_size=auto, caps=SPIRAM, urbs=5, urb_size=8KB }
     * Restored to the backup_before_esp_sr working config (5×8KB URBs).
     * frame_buffers 8 matches the backup; the overflow issue was actually
     * the 12KB frame_handling_task stack (now back to 16KB), not buffer count. */
    .advanced = { 8, 0, MALLOC_CAP_SPIRAM, 5, 8 * 1024, NULL },
};

/* ═══════════════════════════════════════════════════════
 *  WiFi STA 初始化 (ESP-NOW 板间通信)
 * ═══════════════════════════════════════════════════════ */
#define WIFI_SSID      "YOUR_WIFI_SSID"
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"

static esp_err_t wifi_init_sta(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    /* Reduce Wi-Fi buffer counts to save internal SRAM.
     * Default: static_rx=10, dynamic_rx=32, dynamic_tx=32, cache_tx=32, ampdu_rx=1
     * This project only uses Wi-Fi for ESP-NOW + light HTTP (cloud face),
     * so we don't need the full throughput defaults. */
    cfg.static_rx_buf_num    = 2;   // default 10 → 2
    cfg.static_tx_buf_num    = 4;   // default 16 → 4 (static TX is enabled via sdkconfig)
    cfg.dynamic_rx_buf_num   = 4;   // default 32 → 4
    cfg.dynamic_tx_buf_num   = 4;   // default 32 → 4
    cfg.cache_tx_buf_num     = 0;   // default 32 → 0 (saves ~7KB; ESP-NOW doesn't need it)
    cfg.mgmt_sbuf_num        = 16;  // default 32 → 16
    cfg.ampdu_rx_enable      = 0;   // disable AMPDU RX (saves ~6 rx ba win buffers)
    cfg.ampdu_tx_enable      = 0;   // disable AMPDU TX
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "WiFi STA connecting to %s...", WIFI_SSID);

    int retry = 0;
    while (retry < 30) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected to %s, RSSI=%d", ap_info.ssid, ap_info.rssi);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        retry++;
    }
    if (retry >= 30) {
        ESP_LOGE(TAG, "WiFi connection timeout!");
        return ESP_ERR_TIMEOUT;
    }

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    /* Wait for DHCP (IP address) — required for cloud_face HTTPS */
    ESP_LOGI(TAG, "Waiting for IP address...");
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    int ip_retry = 0;
    while (ip_retry < 40) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK &&
            ip_info.ip.addr != 0) {
            ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ip_info.ip));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        ip_retry++;
    }
    if (ip_retry >= 40) {
        ESP_LOGW(TAG, "DHCP timeout, cloud services may fail");
    }

    ESP_LOGI(TAG, "WiFi STA ready, power save disabled");

    /* Start SNTP time sync (Beijing time, UTC+8) */
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP time sync started (TZ=CST-8)");

    return ESP_OK;
}

/* ── 内存诊断：打印各阶段内部/DMA/PSRAM 剩余 + 堆完整性检查 ── */
#define MEM_DUMP(where) do { \
    ESP_LOGI(TAG, "MEM[%s]: internal=%luKB dma=%luKB psram=%luKB free=%luKB", where, \
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024, \
             heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024, \
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024, \
             esp_get_free_heap_size() / 1024); \
    heap_caps_check_integrity_all(true); \
} while (0)

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== FatigueGuard | USB UVC MJPEG + PERCLOS + Face Recognition ===");
    ESP_LOGI(TAG, "RAM: %lu KB  PSRAM: %lu KB",
             esp_get_free_heap_size() / 1024,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    MEM_DUMP("start");

    s_frame_queue = xQueueCreate(FRAME_QUEUE_LEN, sizeof(uvc_host_frame_t *));
    assert(s_frame_queue);

    s_usb_free_sem = xSemaphoreCreateBinary();
    assert(s_usb_free_sem);

    ESP_LOGI(TAG, "Installing USB Host library");
    usb_host_config_t host_config;
    memset(&host_config, 0, sizeof(host_config));
    host_config.skip_phy_setup = false;
    host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    BaseType_t task_ok = xTaskCreatePinnedToCore(
        usb_lib_task, "usb_lib", 4096, NULL, USB_HOST_PRIORITY, NULL, tskNO_AFFINITY);
    assert(task_ok == pdTRUE);

    /* ═══════════════════════════════════════════════════════
     * WiFi init MUST be early — Wi-Fi static RX buffers require
     * internal DRAM (not PSRAM). If called after heavy components
     * (UVC / TFLite / ESP-DL), internal DRAM is fragmented and
     * esp_wifi_init() fails with ESP_ERR_NO_MEM.
     * ═══════════════════════════════════════════════════════ */
    ESP_LOGI(TAG, "Init WiFi STA (early, before heavy allocs)...");
    bool wifi_ok = (wifi_init_sta() == ESP_OK);
    if (wifi_ok) {
        ESP_LOGI(TAG, "Init ESP-NOW protocol...");
        ESP_ERROR_CHECK(esp_now_protocol_init());
    } else {
        ESP_LOGE(TAG, "WiFi init failed! VirtualGPS + cloud face unavailable.");
    }
    MEM_DUMP("after-wifi");

    /* ═══ 语音识别（ESP-SR + UAC 麦克风）已禁用（2026-08-02）═══
     * ESP-SR 模型运行时占 ~73KB 内部 RAM，导致 TFT(LVGL) 任务栈无法分配，
     * 且所有任务栈必须放内部（spi_flash 断言）。取舍：关语音保核心功能。
     * 恢复方法：取消下面两行 + 把 LVGL 栈恢复 8KB（见 lvgl_ui.cpp）。
     */
    // mic_host_init();
    // esp_sr_init();
    MEM_DUMP("after-espsr(skipped)");

    ESP_LOGI(TAG, "Installing UVC driver");
    const uvc_host_driver_config_t uvc_driver_config = {
        .driver_task_stack_size = 4 * 1024,
        .driver_task_priority = USB_HOST_PRIORITY + 1,
        .xCoreID = tskNO_AFFINITY,
        .create_background_task = true,
        .event_cb = NULL,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(uvc_host_install(&uvc_driver_config));
    MEM_DUMP("after-uvc");

    ESP_LOGI(TAG, "Initializing eye classifier...");
    if (!eye_classifier_init()) ESP_LOGE(TAG, "Eye classifier init failed!");
    ESP_LOGI(TAG, "Eye classifier ready");
    MEM_DUMP("after-eyeclf");

    ESP_LOGI(TAG, "Initializing face recognizer (local fallback)...");
    s_face_recog_available = face_recognizer_init();
    if (!s_face_recog_available) {
        ESP_LOGW(TAG, "Face recognizer init failed! Continuing without it...");
    } else {
        int enrolled = face_recognizer_get_enrolled_count();
        if (enrolled == 0) ESP_LOGI(TAG, "No drivers. First face will auto-enroll.");
        else ESP_LOGI(TAG, "Drivers enrolled: %d", enrolled);
    }
    MEM_DUMP("after-facerec");

    /* Cloud face recognition (Baidu AI) — WiFi already connected by now */
    if (wifi_ok) {
        ESP_LOGI(TAG, "Initializing cloud face recognition...");
        if (!cloud_face_init(BAIDU_API_KEY, BAIDU_SECRET_KEY)) {
            ESP_LOGW(TAG, "Cloud face init failed! Will use local fallback.");
        }
    } else {
        ESP_LOGW(TAG, "Cloud face recognition skipped (no WiFi).");
    }

    s_detector = new HumanFaceDetect(HumanFaceDetect::model_type_t::MSRMNP_S8_V1, false);
    assert(s_detector);
    s_detector->set_score_thr(0.3f, 0);  /* Stage 0: 30% confidence */
    s_detector->set_score_thr(0.2f, 1);  /* Stage 1: 20% confidence */
    ESP_LOGI(TAG, "Model loaded (face detect threshold: 0.3/0.2)");

    /* I2S voice prompter disabled — voice alerts via VirtualGPS SYN6288 */
    // voice_prompter_init();

    /* Initialize EG800K UART1 (GPIO17 TX / GPIO18 RX, I2S disabled) */
    ESP_LOGI(TAG, "Init EG800K UART1: TX=17,RX=18...");
    gps_uart_init(18, 17);  // EG800K: RX=18,TX=17

    /* frame_handling_task MUST use internal RAM stack: it calls
     * face_recognizer_identify() → NVS read → spi_flash disables caches,
     * and a PSRAM stack crashes there (esp_task_stack_is_sane_cache_disabled).
     * 12KB overflowed the deep process_frame call chain (stuck frame_q);
     * restore to the original 16KB now that ESP-SR freed internal RAM. */
    task_ok = xTaskCreatePinnedToCore(
        frame_handling_task, "uvc_frame", 16384,
        (void *)&s_stream_config, USB_HOST_PRIORITY - 2, NULL, tskNO_AFFINITY);
    assert(task_ok == pdTRUE);

    ESP_LOGI(TAG, "Init complete. Waiting for camera...");

    /* Init LVGL task infrastructure early (before USB allocates DMA memory) */
    lvgl_ui_init_task();
}
