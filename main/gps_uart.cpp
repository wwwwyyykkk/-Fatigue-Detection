/**
 * gps_uart.cpp - 双向通信: ESP-NOW(VirtualGPS) + UART1(EG800K)
 *
 * ESP-NOW cmd 0x10 RX: G-prefix (gps_data_msg_t) from VirtualGPS
 * ESP-NOW cmd 0x20 TX: F-prefix (fatigue_state_msg_t) to VirtualGPS
 * UART1 RX: G,speed,location,driver,duration,valid[,lat,lng[,weather]] from EG800K
 * UART1 TX: speed,location,driver,dur,fatigue,face,alc,drv,sys to EG800K
 *
 * ⚠️ ESP-NOW 接收回调仅做数据拷贝，不阻塞、不延时。
 */
#include "gps_uart.h"
#include "display_data.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_now_link.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "GPS_UART";

/* ── UART0: VirtualGPS (DISABLED, replaced by ESP-NOW) ── */
#if 0
#define VGPS_UART_NUM   UART_NUM_0
#define VGPS_BAUD_RATE  115200
#define VGPS_BUF_SIZE   256
#endif
#define VGPS_TIMEOUT_MS 10000

/* ── UART1: EG800K (unchanged) ─────────────────────── */
#define EG800K_UART_NUM   UART_NUM_1
#define EG800K_BAUD_RATE  115200
#define EG800K_BUF_SIZE   256

static int64_t s_last_recv_us = 0;
static bool s_connected = false;

/* Rest request from VirtualGPS BUTTON_3 (set in recv_cb, consumed in main loop) */
int g_rest_request_pending = 0;
int g_rest_stop_pending = 0;      /* req=0: stop rest (if not mandatory) */
int g_demo_cmd_pending = -1;      /* Demo Mode from VirtualGPS Rainmaker: 1=start, 0=stop, -1=none */

static float s_speed = 0;
static int   s_location = 0;
static int   s_driver_id = 1;
static int   s_drive_seconds = 0;

/* Rainmaker mock perclos 覆盖 (来自 VirtualGPS MOCK_STATE, -1=不覆盖用真实值) */
static int s_mock_perclos = -1;
static float s_lat = 32.105000f;   /* 南京集成电路培训中心附近 (未收到 VGPS 时 fallback) */
static float s_lng = 118.705000f;
static uint8_t s_gps_mode = 1;  /* 0=真实GPS, 1=模拟mock */

/* ── Forward declare ──────────────────────────────── */
static void parse_g_message(const char *line);

/* ═══════════════════════════════════════════════════════
 *  ESP-NOW receive callback (cmd 0x10: VirtualGPS → Main)
 *  ⚠️ 仅做数据拷贝！绝不可阻塞、延时、分配内存！
 * ═══════════════════════════════════════════════════════ */
static void esp_now_recv_cb(const uint8_t *mac, esp_now_msg_t *msg)
{
    if (msg->cmd == ESP_NOW_CMD_GPS_DATA && msg->data_len == sizeof(gps_data_msg_t)) {
        gps_data_msg_t *gps = (gps_data_msg_t *)msg->data;

        s_speed         = gps->speed;
        s_location      = gps->location;
        s_driver_id     = gps->driver_id;
        s_drive_seconds = gps->drive_sec;
        s_lat           = gps->lat;
        s_lng           = gps->lng;
        s_gps_mode      = gps->gps_mode;
        s_last_recv_us  = esp_timer_get_time();

        if (!s_connected) {
            ESP_LOGI(TAG, "ESP-NOW VirtualGPS connected: %.0f km/h loc=%d drv=%d dur=%ds",
                     s_speed, s_location, s_driver_id, s_drive_seconds);
        }
        s_connected = true;
    } else if (msg->cmd == ESP_NOW_CMD_REST && msg->data_len >= 1) {
        /* VirtualGPS BUTTON_3 → rest request (req=1=start, req=0=stop).
         * Set flag; main loop processes in start_rest()/stop_rest(). */
        uint8_t req = msg->data[0];
        if (req == 1) g_rest_request_pending = 1;
        else if (req == 0) g_rest_stop_pending = 1;
    } else if (msg->cmd == ESP_NOW_CMD_MOCK_STATE && msg->data_len == sizeof(mock_state_msg_t)) {
        /* Rainmaker mock perclos → 覆盖上报给 EG800K 的值(不改真实检测/TFT) */
        mock_state_msg_t *mock = (mock_state_msg_t *)msg->data;
        s_mock_perclos = (int)mock->perclos;   /* -1=不覆盖 */
        ESP_LOGI(TAG, "ESP-NOW MOCK: perclos override=%d", s_mock_perclos);
    } else if (msg->cmd == ESP_NOW_CMD_DEMO && msg->data_len >= 1) {
        /* VirtualGPS "Demo Mode" 按钮 → 启动/停止主板疲劳预测演示 */
        uint8_t d = msg->data[0];
        g_demo_cmd_pending = d ? 1 : 0;
        ESP_LOGI(TAG, "ESP-NOW DEMO: %s (cmd=0x51)", d ? "ON" : "OFF");
    }
}

/* ── ESP-NOW initialized flag ── */
static bool g_esp_now_ready = false;

/* ═══════════════════════════════════════════════════════
 *  ESP-NOW init (called after WiFi connected in main.cpp)
 * ═══════════════════════════════════════════════════════ */
esp_err_t esp_now_protocol_init(void)
{
    esp_now_link_set_recv_cb(esp_now_recv_cb);
    esp_err_t ret = esp_now_link_init();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ESP-NOW protocol init OK (VGPS ← Main)");
        g_esp_now_ready = true;
    }
    return ret;
}

/* ═══════════════════════════════════════════════════════
 *  ESP-NOW send F-state (cmd 0x20: Main → VirtualGPS)
 * ═══════════════════════════════════════════════════════ */
void esp_now_protocol_send_f_state(int fatigue_level, int face_verified,
                                   int alcohol_detected, int driver_confirmed,
                                   int system_status, int identify_event,
                                   int pred_minutes, int pred_level)
{
    if (!g_esp_now_ready) return;

    fatigue_state_msg_t f_msg = {
        .fatigue_level    = (uint8_t)fatigue_level,
        .face_verified    = (uint8_t)face_verified,
        .alcohol          = (uint8_t)alcohol_detected,
        .driver_confirmed = (uint8_t)driver_confirmed,
        .sys_status       = (uint8_t)system_status,
        .identify_event   = (uint8_t)identify_event,
        .pred_minutes     = (int16_t)pred_minutes,
        .pred_level       = (uint8_t)pred_level,
    };
    esp_now_link_send(ESP_NOW_CMD_FATIGUE, (uint8_t *)&f_msg, sizeof(f_msg));
    ESP_LOGI(TAG, "ESP-NOW TX F: fatigue=%d face=%d alc=%d drv=%d sys=%d id_evt=%d pred=%dmin/%d",
             f_msg.fatigue_level, f_msg.face_verified, f_msg.alcohol,
             f_msg.driver_confirmed, f_msg.sys_status, f_msg.identify_event,
             f_msg.pred_minutes, f_msg.pred_level);
}

/* ═══════════════════════════════════════════════════════
 *  ESP-NOW poll (替代 gps_uart_poll)
 *  - 检查超时断开
 *  - 同步 g_display_data.gps_*
 * ═══════════════════════════════════════════════════════ */
void esp_now_protocol_poll(void)
{
    if (!g_esp_now_ready) return;

    /* Check timeout */
    if (s_connected) {
        int64_t now = esp_timer_get_time();
        if ((now - s_last_recv_us) / 1000 > VGPS_TIMEOUT_MS) {
            s_connected = false;
            ESP_LOGW(TAG, "VirtualGPS ESP-NOW disconnected");
        }
    }

    /* Update display data */
    extern display_data_t g_display_data;
    g_display_data.gps_speed         = s_speed;
    g_display_data.gps_location      = s_location;
    g_display_data.gps_driver_id     = s_driver_id;
    g_display_data.gps_drive_seconds = s_drive_seconds;
    g_display_data.gps_lat           = s_lat;
    g_display_data.gps_lng           = s_lng;
    g_display_data.gps_connected     = s_connected;
    g_display_data.gps_mode          = s_gps_mode;   /* 0=real, 1=mock */
}

/* ── Parse G-prefix text message (still used by EG800K UART1) ── */
static void parse_g_message(const char *line)
{
    if (line[0] != 'G' || line[1] != ',') return;
    float speed; int loc, drv, sec, valid;
    float lat = 0, lng = 0;
    int weather = -1;  // -1 = not present in message
    int n = sscanf(line + 2, "%f,%d,%d,%d,%d,%f,%f,%d",
                   &speed, &loc, &drv, &sec, &valid, &lat, &lng, &weather);
    if (n < 5) return;  // minimum: G,speed,loc,drv,dur,valid

    s_speed = speed; s_location = loc;
    s_driver_id = drv; s_drive_seconds = sec;
    /* 坐标以 VirtualGPS ESP-NOW 为准(如南京)。EG800K 回传仅当 VGPS 断开时兜底，
     * 否则会被 EG800K 的旧轨迹坐标覆盖，导致网页显示福州而非南京。 */
    if (n >= 7 && !s_connected) { s_lat = lat; s_lng = lng; }
    s_last_recv_us = esp_timer_get_time();

    /* Weather risk auto-sync (field 9, EG800K appends from Flask polling).
     * Signal file-scope in main.cpp; the main loop applies it to PerclosCalculator. */
    extern int g_weather_risk_raw;
    extern int g_weather_pending;
    if (weather >= 0 && weather <= 2) {
        if (weather != g_weather_risk_raw) {
            g_weather_risk_raw = weather;
            g_weather_pending = 1;
            ESP_LOGI(TAG, "Weather auto-update: risk=%d → PERCLOS mild=%.0f%%",
                     weather,
                     weather == 0 ? 15.0f : (weather == 1 ? 12.0f : 10.0f));
        }
    }

    if (!s_connected) ESP_LOGI(TAG, "VirtualGPS connected: %.0f km/h loc=%d drv=%d dur=%ds",
                                speed, loc, drv, sec);
    s_connected = true;
}

/* ═══════════════════════════════════════════════════════
 *  UART init: EG800K UART1 only (UART0 VGPS → ESP-NOW)
 * ═══════════════════════════════════════════════════════ */
void gps_uart_init(int eg800k_rx, int eg800k_tx)
{
#if 0
    /* ── UART0: VirtualGPS bidirectional -- DISABLED, replaced by ESP-NOW ── */
    uart_config_t cfg0 = {
        .baud_rate = VGPS_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(VGPS_UART_NUM, &cfg0));
    ESP_ERROR_CHECK(uart_set_pin(VGPS_UART_NUM, vgps_tx, vgps_rx,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(VGPS_UART_NUM, VGPS_BUF_SIZE * 2,
                                        VGPS_BUF_SIZE, 0, NULL, 0));
    s_last_recv_us = esp_timer_get_time();
    ESP_LOGI(TAG, "VGPS UART0: TX=GPIO%d RX=GPIO%d %d bps",
             vgps_tx, vgps_rx, VGPS_BAUD_RATE);
#endif

    /* ── UART1: EG800K bidirectional ── */
    uart_config_t cfg1 = {
        .baud_rate = EG800K_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(EG800K_UART_NUM, &cfg1));
    ESP_ERROR_CHECK(uart_set_pin(EG800K_UART_NUM, eg800k_tx, eg800k_rx,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(EG800K_UART_NUM, EG800K_BUF_SIZE * 2,
                                        EG800K_BUF_SIZE, 0, NULL, 0));
    ESP_LOGI(TAG, "EG800K UART1: TX=GPIO%d RX=GPIO%d %d bps",
             eg800k_tx, eg800k_rx, EG800K_BAUD_RATE);
    ESP_LOGI(TAG, "VGPS: using ESP-NOW (UART0 freed for PC debug)");
}

/* ── UART0 poll (VirtualGPS) -- DISABLED, replaced by esp_now_protocol_poll() ── */
#if 0
void gps_uart_poll(void)
{
    uint8_t buf[128];
    int len = uart_read_bytes(VGPS_UART_NUM, buf, sizeof(buf) - 1, pdMS_TO_TICKS(10));
    if (len <= 0) goto check_timeout;

    static char line_buf[128];
    static int  line_pos = 0;
    for (int i = 0; i < len; i++) {
        char c = (char)buf[i];
        if (c == '\n' || c == '\r') {
            if (line_pos > 0 && line_pos < (int)sizeof(line_buf)) {
                line_buf[line_pos] = '\0';
                parse_g_message(line_buf);
                line_pos = 0;
            }
        } else if (line_pos < (int)sizeof(line_buf) - 1) {
            line_buf[line_pos++] = c;
        } else { line_pos = 0; }
    }

check_timeout:
    if (s_connected) {
        int64_t now = esp_timer_get_time();
        if ((now - s_last_recv_us) / 1000 > VGPS_TIMEOUT_MS) {
            s_connected = false;
            ESP_LOGW(TAG, "VirtualGPS disconnected");
        }
    }

    extern display_data_t g_display_data;
    g_display_data.gps_speed         = s_speed;
    g_display_data.gps_location      = s_location;
    g_display_data.gps_driver_id     = s_driver_id;
    g_display_data.gps_drive_seconds = s_drive_seconds;
    g_display_data.gps_lat           = s_lat;
    g_display_data.gps_lng           = s_lng;
    g_display_data.gps_connected     = s_connected;
    g_display_data.gps_mode          = s_gps_mode;   /* 0=real, 1=mock */
}
#endif

/* ── UART1 poll: receive GPS G-prefix from EG800K (unchanged) ── */
void gps_uart_poll_eg800k(void)
{
    uint8_t buf[128];
    int len = uart_read_bytes(EG800K_UART_NUM, buf, sizeof(buf) - 1, pdMS_TO_TICKS(5));
    if (len <= 0) return;

    static char line_buf[128];
    static int  line_pos = 0;
    for (int i = 0; i < len; i++) {
        char c = (char)buf[i];
        if (c == '\n' || c == '\r') {
            if (line_pos > 0 && line_pos < (int)sizeof(line_buf)) {
                line_buf[line_pos] = '\0';
                parse_g_message(line_buf);  /* Same G-prefix parser */
                line_pos = 0;
            }
        } else if (line_pos < (int)sizeof(line_buf) - 1) {
            line_buf[line_pos++] = c;
        } else { line_pos = 0; }
    }
}

/* ── UART2 send F-prefix to VirtualGPS -- DISABLED, replaced by ESP-NOW ── */
#if 0
void gps_uart_send_state(int fatigue_level, int face_verified,
                         int alcohol_detected, int driver_confirmed,
                         int system_status)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "F,%d,%d,%d,%d,%d\n",
             fatigue_level, face_verified, alcohol_detected,
             driver_confirmed, system_status);
    uart_write_bytes(VGPS_UART_NUM, buf, strlen(buf));
    ESP_LOGI(TAG, "TX F(to VGPS): fatigue=%d face=%d alc=%d drv=%d sys=%d",
             fatigue_level, face_verified, alcohol_detected,
             driver_confirmed, system_status);
}
#endif

/* ── UART1 send combined data to EG800K ── */
void gps_uart_send_to_eg800k(int fatigue_level, int face_verified,
                             int driver_confirmed, int system_status,
                             float perclos, int alcohol,
                             int rest_dur_min, int rest_recov_pct)
{
    /* Rainmaker mock 覆盖: -1=用主板真实值, 否则用 mock(只影响上报,不改真实检测) */
    float pcl = (s_mock_perclos >= 0) ? (float)s_mock_perclos : perclos;
    char buf[200];
    snprintf(buf, sizeof(buf), "%.1f,%d,%d,%d,%d,%d,%d,%d,%d,%.1f,%.6f,%.6f,%d,%d,%d\n",
             s_speed,
             s_location,
             s_driver_id,
             s_drive_seconds,
             fatigue_level,
             face_verified,
             alcohol,
             driver_confirmed,
             system_status,
             pcl,
             s_lat,
             s_lng,
             s_gps_mode,
             rest_dur_min,
             rest_recov_pct);
    uart_write_bytes(EG800K_UART_NUM, buf, strlen(buf));
    ESP_LOGI(TAG, "TX to EG800K: %s", buf);
}

bool gps_uart_is_connected(void)     { return s_connected; }
float gps_uart_get_speed(void)       { return s_speed; }
int   gps_uart_get_location(void)    { return s_location; }
int   gps_uart_get_driver_id(void)   { return s_driver_id; }
int   gps_uart_get_drive_seconds(void) { return s_drive_seconds; }
int   gps_uart_get_mock_perclos(void)  { return s_mock_perclos; }
float gps_uart_get_lat(void)         { return s_lat; }
float gps_uart_get_lng(void)         { return s_lng; }
