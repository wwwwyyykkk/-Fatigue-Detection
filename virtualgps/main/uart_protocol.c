/**
 * uart_protocol.c - 双向通信协议 (ESP-NOW 模式)
 *
 * 原 UART2 有线通信已替换为 ESP-NOW WiFi 无线通信。
 * 发送(G): gps_data_msg_t via cmd 0x10  每秒+状态变化
 * 接收(F): fatigue_state_msg_t via cmd 0x20  疲劳/人脸/系统状态
 *
 * ⚠️ ESP-NOW 接收回调仅做数据拷贝，疲劳回调延迟到 uart_poll_receive() 主循环执行
 */
#include "uart_protocol.h"
#include "gps_state.h"
#include "config.h"
#include "app_priv.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_now_link.h"
#include "syn6288.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "UART";

/* Global main board state */
mainboard_state_t g_mainboard_state = {0};
bool g_in_rest = false;   /* tracked from F-prefix identify_event (REST_START/REST_DONE) */

/* Fatigue change callback */
static fatigue_change_cb_t s_fatigue_cb = NULL;

/* ── Deferred callbacks (set in ESP-NOW recv cb, processed in main loop) ── */
static volatile int g_fatigue_changed = 0;
static volatile int g_old_fatigue = 0;
static volatile int g_new_fatigue = 0;
static volatile int g_driver_confirmed = 0;  /* 新确认的司机ID，0=无变化 */
static volatile int g_identify_event = 0;    /* 身份验证事件 IDENTIFY_EVENT_* */
static volatile int g_pred_changed = 0;      /* 预测变化 → 播预测语音 */
static volatile int g_pred_minutes = 0;
static volatile int g_pred_level = 0;
static int s_last_pred_min = -1;

void uart_set_fatigue_change_callback(fatigue_change_cb_t cb) {
    s_fatigue_cb = cb;
}

/* ── Parse F-prefix message (kept for reference, not used in ESP-NOW mode) ── */
#if 0
static void parse_f_message(const char *line)
{
    if (line[0] != 'F' || line[1] != ',') return;
    int fatigue, face_ok, alcohol, drv, sys;
    int n = sscanf(line + 2, "%d,%d,%d,%d,%d", &fatigue, &face_ok, &alcohol, &drv, &sys);
    if (n < 5) return;

    int old = g_mainboard_state.fatigue_level;
    g_mainboard_state.fatigue_level    = fatigue;
    g_mainboard_state.face_verified    = face_ok;
    g_mainboard_state.alcohol_detected = alcohol;
    g_mainboard_state.driver_confirmed = drv;
    g_mainboard_state.system_status    = sys;
    g_mainboard_state.valid            = true;

    ESP_LOGI(TAG, "RX F: fatigue=%d face=%d alc=%d drv=%d sys=%d",
             fatigue, face_ok, alcohol, drv, sys);

    if (s_fatigue_cb && old != fatigue) {
        s_fatigue_cb(old, fatigue);
    }
}
#endif

/* ═══════════════════════════════════════════════════════
 *  ESP-NOW receive callback (cmd 0x20: Main → VirtualGPS)
 *  ⚠️ 仅做数据拷贝！绝不可调 syn6288_speak / 阻塞 / 延时！
 * ═══════════════════════════════════════════════════════ */
static void esp_now_recv_cb(const uint8_t *mac, esp_now_msg_t *msg)
{
    if (msg->cmd == ESP_NOW_CMD_FATIGUE && msg->data_len == sizeof(fatigue_state_msg_t)) {
        fatigue_state_msg_t *fm = (fatigue_state_msg_t *)msg->data;

        int old = g_mainboard_state.fatigue_level;
        int old_drv = g_mainboard_state.driver_confirmed;

        g_mainboard_state.fatigue_level    = fm->fatigue_level;
        g_mainboard_state.face_verified    = fm->face_verified;
        g_mainboard_state.alcohol_detected = fm->alcohol;
        g_mainboard_state.driver_confirmed = fm->driver_confirmed;
        g_mainboard_state.system_status    = fm->sys_status;
        g_mainboard_state.valid            = true;

        ESP_LOGI(TAG, "ESP-NOW RX F: fatigue=%d face=%d alc=%d drv=%d sys=%d",
                 fm->fatigue_level, fm->face_verified, fm->alcohol,
                 fm->driver_confirmed, fm->sys_status);

        /* Report to RainMaker cloud */
        rainmaker_report_fatigue_state(fm->fatigue_level, fm->face_verified,
                                        fm->alcohol, fm->driver_confirmed,
                                        fm->sys_status);

        /* Defer callback to main loop — syn6288_speak MUST NOT run in recv context */
        if (old != fm->fatigue_level) {
            g_fatigue_changed = 1;
            g_old_fatigue = old;
            g_new_fatigue = fm->fatigue_level;
        }

        /* Prediction → voice (deferred, only on change): "预计X分钟后进入轻度/严重疲劳" */
        int pmin = (int)fm->pred_minutes;
        if (pmin > 0 && pmin != s_last_pred_min) {
            s_last_pred_min = pmin;
            g_pred_minutes = pmin;
            g_pred_level   = fm->pred_level;
            g_pred_changed = 1;
        } else if (pmin <= 0) {
            s_last_pred_min = -1;   /* 无预测: 重置, 下次有新预测再播 */
        }

        /* Driver confirmed change → voice in main loop */
        if (fm->driver_confirmed > 0 && fm->driver_confirmed != old_drv) {
            g_driver_confirmed = fm->driver_confirmed;
        }

        /* Identify event → voice in main loop */
        if (fm->identify_event != IDENTIFY_EVENT_NONE) {
            g_identify_event = fm->identify_event;
        }

        /* Track rest state for BUTTON_3 toggle */
        if (fm->identify_event == IDENTIFY_EVENT_REST_START) {
            g_in_rest = true;
        } else if (fm->identify_event == IDENTIFY_EVENT_REST_DONE) {
            g_in_rest = false;
        }
    }
}

/* ── ESP-NOW initialized flag ── */
static bool g_esp_now_ready = false;

/* ═══════════════════════════════════════════════════════
 *  ESP-NOW init (called after WiFi connected)
 * ═══════════════════════════════════════════════════════ */
esp_err_t esp_now_protocol_init_wifi(void)
{
    esp_now_link_set_recv_cb(esp_now_recv_cb);
    esp_err_t ret = esp_now_link_init();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ESP-NOW protocol init OK (VGPS → Main)");
        g_esp_now_ready = true;
    }
    return ret;
}

/* ── Poll for incoming F-prefix data (ESP-NOW mode: process deferred fatigue cb) ── */
void uart_poll_receive(void)
{
#if 0
    /* ── UART2 read -- DISABLED, replaced by ESP-NOW callback ── */
    uint8_t buf[64];
    int len = uart_read_bytes(UART_PORT_NUM, buf, sizeof(buf) - 1, pdMS_TO_TICKS(5));
    if (len <= 0) return;

    static char line_buf[64];
    static int  line_pos = 0;

    for (int i = 0; i < len; i++) {
        char c = (char)buf[i];
        if (c == '\n' || c == '\r') {
            if (line_pos > 0) {
                line_buf[line_pos] = '\0';
                parse_f_message(line_buf);
                line_pos = 0;
            }
        } else if (line_pos < (int)sizeof(line_buf) - 1) {
            line_buf[line_pos++] = c;
        } else {
            line_pos = 0;
        }
    }
#endif

    /* Process deferred fatigue change from ESP-NOW recv callback.
     * ⚠️ This runs in main loop context — safe to call syn6288_speak. */
    if (g_fatigue_changed) {
        g_fatigue_changed = 0;
        if (s_fatigue_cb) {
            s_fatigue_cb(g_old_fatigue, g_new_fatigue);
        }
    }

    /* Process deferred driver confirmation → voice */
    if (g_driver_confirmed) {
        int drv = g_driver_confirmed;
        g_driver_confirmed = 0;
        if (drv == 1) {
            syn6288_speak(SYN_VOICE_DRIVER1_OK);
        } else if (drv == 2) {
            syn6288_speak(SYN_VOICE_DRIVER2_OK);
        }
    }

    /* Process deferred identify event → voice */
    if (g_identify_event) {
        int evt = g_identify_event;
        g_identify_event = 0;
        if (evt == IDENTIFY_EVENT_START) {
            syn6288_speak(SYN_VOICE_IDENTIFY_START);   /* 主板开始人脸检测 → 播报 */
        } else if (evt == IDENTIFY_EVENT_FAIL) {
            syn6288_speak(SYN_VOICE_IDENTIFY_FAIL);
        } else if (evt == IDENTIFY_EVENT_REST_DONE) {
            syn6288_speak(SYN_VOICE_REST_DONE);   /* 休息时间已满，可以继续出发 */
        }
        /* REST_START (4) 由本地 BUTTON_3 触发时已播 SYN_VOICE_REST_START，
         * 这里不重复播（避免与本地语音叠加）。
         * SUCCESS (2) 走 g_driver_confirmed 播 DRIVER*_OK，不在此重复。 */
    }

    /* Process deferred prediction → voice: "预计X分钟后进入轻度/严重疲劳" */
    if (g_pred_changed) {
        g_pred_changed = 0;
        syn6288_speak_prediction(g_pred_minutes, g_pred_level);
    }
}

/* ═══════════════════════════════════════════════════════
 *  UART init: ESP-NOW mode (UART2 hardware disabled)
 * ═══════════════════════════════════════════════════════ */
void uart_protocol_init(void)
{
#if 0
    /* ── UART2 hardware init -- DISABLED, replaced by ESP-NOW ── */
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE * 2,
                                        UART_BUF_SIZE, 0, NULL, 0));

    ESP_LOGI(TAG, "UART%d TX=GPIO%d RX=GPIO%d %d bps (bidirectional)",
             UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN, UART_BAUD_RATE);
#endif
    ESP_LOGI(TAG, "Protocol using ESP-NOW (UART2 disabled)");
}

/* ═══════════════════════════════════════════════════════
 *  Send G-prefix → ESP-NOW cmd 0x10 (VirtualGPS → 主控板)
 * ═══════════════════════════════════════════════════════ */
void uart_send_now(void)
{
    if (!g_esp_now_ready) return;  /* WiFi/ESP-NOW not up yet, skip */

    virtual_gps_state_t state;
    gps_state_read(&state);

    gps_data_msg_t g_msg = {
        .speed     = state.speed_kmh,
        .location  = (uint8_t)state.location,
        .driver_id = (uint8_t)state.driver_id,
        .drive_sec = (int32_t)state.drive_seconds,
        .lat       = state.gps_lat,
        .lng       = state.gps_lng,
        .gps_mode  = state.use_real_gps ? 0 : 1,  /* 0=真实 1=模拟 */
    };
    esp_now_link_send(ESP_NOW_CMD_GPS_DATA, (uint8_t *)&g_msg, sizeof(g_msg));

#if 0
    /* ── UART text-based send -- DISABLED, replaced by ESP-NOW binary ── */
    char buf[80];
    snprintf(buf, sizeof(buf), "G,%.1f,%d,%d,%d,1,%.6f,%.6f\n",
             state.speed_kmh,
             (int)state.location,
             state.driver_id,
             state.drive_seconds,
             state.gps_lat,
             state.gps_lng);

    uart_write_bytes(UART_PORT_NUM, buf, strlen(buf));
    ESP_LOGI(TAG, "TX: %s", buf);
#endif

    ESP_LOGI(TAG, "ESP-NOW TX: speed=%.1f loc=%d drv=%d sec=%d lat=%.6f lng=%.6f",
             g_msg.speed, g_msg.location, g_msg.driver_id,
             g_msg.drive_sec, g_msg.lat, g_msg.lng);
}

/* ── Rest request to main board ───────────────────────── */
void uart_send_rest_request(void)
{
    if (!g_esp_now_ready) { ESP_LOGW(TAG, "ESP-NOW not ready, skip rest request"); return; }
    uint8_t req = 1;
    esp_now_link_send(ESP_NOW_CMD_REST, &req, sizeof(req));
    ESP_LOGI(TAG, "ESP-NOW TX: REST REQUEST sent (cmd=0x30, req=1)");
}

/* ── Rest stop to main board ─────────────────────────── */
void uart_send_rest_stop(void)
{
    if (!g_esp_now_ready) { ESP_LOGW(TAG, "ESP-NOW not ready, skip rest stop"); return; }
    uint8_t req = 0;
    esp_now_link_send(ESP_NOW_CMD_REST, &req, sizeof(req));
    ESP_LOGI(TAG, "ESP-NOW TX: REST STOP sent (cmd=0x30, req=0)");
}

/* ── Rainmaker mock state → main board ─────────────────── */
void uart_send_mock_state(void)
{
    if (!g_esp_now_ready) return;

    /* 引用 app_priv.c 中声明的 mock 全局变量 */
    extern int g_mock_weather_risk;
    extern int g_mock_perclos;
    extern int g_mock_fatigue_level;
    extern int g_mock_alcohol;
    extern int g_mock_face_verified;

    mock_state_msg_t mock;
    mock.weather_risk  = (uint8_t)g_mock_weather_risk;
    mock.perclos       = (int16_t)g_mock_perclos;   /* -1=不覆盖 */
    mock.fatigue_level = (uint8_t)g_mock_fatigue_level;
    mock.alcohol       = (uint8_t)g_mock_alcohol;
    mock.face_verified = (uint8_t)g_mock_face_verified;
    mock.reserved      = 0;

    esp_now_link_send(ESP_NOW_CMD_MOCK_STATE, (uint8_t *)&mock, sizeof(mock));
}

/* ── Demo ramp command → main board ──────────────────── */
void uart_send_demo_cmd(int on)
{
    if (!g_esp_now_ready) { ESP_LOGW(TAG, "ESP-NOW not ready, skip demo cmd"); return; }
    uint8_t d = on ? 1 : 0;
    esp_now_link_send(ESP_NOW_CMD_DEMO, &d, sizeof(d));
    ESP_LOGI(TAG, "ESP-NOW TX: DEMO %s (cmd=0x51)", on ? "ON" : "OFF");
}

/* ── Heartbeat task ──────────────────────────────────── */
static void uart_heartbeat_task(void *arg)
{
    ESP_LOGI(TAG, "Heartbeat task started (%dms)", UART_HEARTBEAT_INTERVAL_MS);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(UART_HEARTBEAT_INTERVAL_MS));
        uart_send_now();
        uart_send_mock_state();
    }
}

void uart_start_heartbeat_task(void)
{
    xTaskCreate(uart_heartbeat_task, "uart_hb", 4096, NULL, 3, NULL);
}
