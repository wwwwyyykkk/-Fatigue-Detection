/**
 * app_priv.c — VirtualGPS RainMaker 设备层实现
 *
 * 三个 RainMaker 设备：
 *   1. "GPS State"  — 只读, 上报速度/位置/司机/坐标/GPS来源
 *   2. "Fatigue"    — 只读, 上报疲劳等级/人脸/酒精/系统状态
 *   3. "Control"    — 读写, 远程触发 TTS 语音/切换司机
 *
 * 参考: ESP32S3/esp-rainmaker/examples/switch/main/app_main.c
 */

#include "app_priv.h"
#include "gps_state.h"
#include "syn6288.h"
#include "uart_protocol.h"
#include "buttons_gpio.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include <esp_rmaker_common_events.h>
#include <esp_rmaker_ota.h>
#include <app_network.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "RM_DEV";

/* ── 全局设备指针 ─────────────────────────────────────── */
esp_rmaker_device_t *gps_device     = NULL;
esp_rmaker_device_t *fatigue_device = NULL;
esp_rmaker_device_t *control_device = NULL;

/* ── 本地缓存（避免重复上报相同值） ───────────────────── */
static float    g_last_speed         = -1.0f;
static int      g_last_location      = -1;
static int      g_last_driver_id     = -1;
static int      g_last_drive_min     = -1;  /* minutes, not seconds */
static float    g_last_lat           = -999.0f;
static float    g_last_lng           = -999.0f;
static int      g_last_gps_src       = -1;
static int      g_last_fatigue       = -1;
static int      g_last_face          = -1;
static int      g_last_alcohol       = -1;
static int      g_last_drv_conf      = -1;
static int      g_last_sys           = -1;

/* ── Mock 变量（Rainmaker Control 面板模拟触发，心跳时上发主板） ──
 * g_mock_perclos = -1 表示不覆盖(用主板真实值);App 设 PERCLOS 后 >=0 覆盖上报值 */
int g_mock_weather_risk   = 0;
int g_mock_perclos        = -1;
int g_mock_fatigue_level  = 0;
int g_mock_alcohol        = 0;
int g_mock_face_verified  = 0;

/* ═══════════════════════════════════════════════════════
 *  write_cb — 云端下发控制命令
 * ═══════════════════════════════════════════════════════ */
static esp_err_t write_cb(const esp_rmaker_device_t *device,
                          const esp_rmaker_param_t *param,
                          const esp_rmaker_param_val_t val,
                          void *priv_data,
                          esp_rmaker_write_ctx_t *ctx)
{
    const char *param_name = esp_rmaker_param_get_name(param);
    const char *dev_name   = esp_rmaker_device_get_name(device);

    if (ctx) {
        ESP_LOGI(TAG, "Write from: %s", esp_rmaker_device_cb_src_to_str(ctx->src));
    }

    /* ── 原有控件 ─────────────────────────────────────── */
    if (strcmp(param_name, "Voice Alert") == 0) {
        int voice_id = val.val.i;
        ESP_LOGI(TAG, "Cloud cmd: Voice Alert = %d", voice_id);
        if (voice_id >= 0 && voice_id < SYN_VOICE_COUNT) {
            syn6288_speak((syn_voice_id_t)voice_id);
        }
        esp_rmaker_param_update(param, val);

    } else if (strcmp(param_name, "Driver Switch") == 0) {
        ESP_LOGI(TAG, "Cloud cmd: Driver Switch = %d", val.val.b ? 1 : 0);
        if (val.val.b) {  /* 只在打开时切换 */
            gps_state_toggle_driver();
            /* 与物理按键 BUTTON_5 一致：播报切换 + 身份验证提示 */
            syn6288_speak(SYN_VOICE_DRIVER_SWITCH);
            syn6288_speak(SYN_VOICE_IDENTIFY_START);
            virtual_gps_state_t state;
            gps_state_read(&state);
            rainmaker_report_gps_state(state.speed_kmh, (int)state.location,
                                        state.driver_id, state.drive_seconds,
                                        state.gps_lat, state.gps_lng, state.use_real_gps);
        }
        esp_rmaker_param_update(param, val);

    /* ── 新增控件 ─────────────────────────────────────── */
    } else if (strcmp(param_name, "Speed") == 0) {
        float speed = (float)val.val.i;
        ESP_LOGI(TAG, "Cloud cmd: Speed = %.0f", speed);
        gps_state_set_speed(speed);
        if (speed >= 150.0f) syn6288_speak(SYN_VOICE_OVERSPEED);
        uart_send_now();
        {
            virtual_gps_state_t st;
            gps_state_read(&st);
            rainmaker_report_gps_state(st.speed_kmh, (int)st.location,
                                        st.driver_id, st.drive_seconds,
                                        st.gps_lat, st.gps_lng, st.use_real_gps);
        }
        esp_rmaker_param_update(param, val);

    } else if (strcmp(param_name, "Location") == 0) {
        int loc = val.val.i;
        ESP_LOGI(TAG, "Cloud cmd: Location = %d", loc);
        gps_state_set_location(loc);
        if (loc == 1) syn6288_speak(SYN_VOICE_SERVICE_AREA);
        else if (loc == 2) syn6288_speak(SYN_VOICE_EMERGENCY_LANE);
        uart_send_now();
        {
            virtual_gps_state_t st;
            gps_state_read(&st);
            rainmaker_report_gps_state(st.speed_kmh, (int)st.location,
                                        st.driver_id, st.drive_seconds,
                                        st.gps_lat, st.gps_lng, st.use_real_gps);
        }
        esp_rmaker_param_update(param, val);

    } else if (strcmp(param_name, "Weather Risk") == 0) {
        g_mock_weather_risk = val.val.i;
        ESP_LOGI(TAG, "Cloud cmd: Weather Risk = %d", g_mock_weather_risk);
        esp_rmaker_param_update(param, val);

    } else if (strcmp(param_name, "Fatigue Level") == 0) {
        int new_level = val.val.i;
        ESP_LOGI(TAG, "Cloud cmd: Fatigue Level = %d", new_level);
        g_mock_fatigue_level = new_level;
        /* 直接触发 TTS 播报 */
        if (new_level == 1) syn6288_speak(SYN_VOICE_MILD_FATIGUE);
        else if (new_level == 2) syn6288_speak(SYN_VOICE_SEVERE_FATIGUE);
        esp_rmaker_param_update(param, val);

    } else if (strcmp(param_name, "PERCLOS") == 0) {
        g_mock_perclos = val.val.i;
        ESP_LOGI(TAG, "Cloud cmd: PERCLOS = %d%%", g_mock_perclos);
        uart_send_mock_state();   /* 立即上发主板覆盖上报值(不等心跳) */
        esp_rmaker_param_update(param, val);

    } else if (strcmp(param_name, "Reset Perclos") == 0) {
        ESP_LOGI(TAG, "Cloud cmd: Reset Perclos = %d", val.val.b ? 1 : 0);
        if (val.val.b) {  /* 只在打开时重置 */
            g_mock_perclos = -1;         /* 清除覆盖, 用主板真实值 */
            g_mock_fatigue_level = 0;
            uart_send_mock_state();      /* 立即通知主板恢复 */
        }
        esp_rmaker_param_update(param, val);

    } else if (strcmp(param_name, "Alcohol") == 0) {
        g_mock_alcohol = val.val.b ? 1 : 0;
        ESP_LOGI(TAG, "Cloud cmd: Alcohol = %d", g_mock_alcohol);
        if (g_mock_alcohol) syn6288_speak(SYN_VOICE_ALCOHOL_WARNING);
        esp_rmaker_param_update(param, val);

    } else if (strcmp(param_name, "Face Verified") == 0) {
        g_mock_face_verified = val.val.b ? 1 : 0;
        ESP_LOGI(TAG, "Cloud cmd: Face Verified = %d", g_mock_face_verified);
        esp_rmaker_param_update(param, val);

    } else if (strcmp(param_name, "+30 Min") == 0) {
        ESP_LOGI(TAG, "Cloud cmd: +30 Min = %d", val.val.b ? 1 : 0);
        if (val.val.b) {  /* 只在打开时 +30min */
            int total_min = gps_state_add_drive_time(30);
            if (total_min >= 240) syn6288_speak(SYN_VOICE_DRIVE_4H);
            else if (total_min >= 210) syn6288_speak(SYN_VOICE_DRIVE_3_5H);
            else if (total_min >= 180) syn6288_speak(SYN_VOICE_DRIVE_3H);
            uart_send_now();
            {
                virtual_gps_state_t st;
                gps_state_read(&st);
                rainmaker_report_gps_state(st.speed_kmh, (int)st.location,
                                            st.driver_id, st.drive_seconds,
                                            st.gps_lat, st.gps_lng, st.use_real_gps);
            }
        }
        esp_rmaker_param_update(param, val);

    } else if (strcmp(param_name, "Rest Request") == 0) {
        ESP_LOGI(TAG, "Cloud cmd: Rest Request = %d", val.val.b ? 1 : 0);
        if (val.val.b) {  /* 只在打开时请求休息 */
            buttons_handle_rest_request();
        }
        esp_rmaker_param_update(param, val);

    } else if (strcmp(param_name, "Demo Mode") == 0) {
        ESP_LOGI(TAG, "Cloud cmd: Demo Mode = %d", val.val.b ? 1 : 0);
        uart_send_demo_cmd(val.val.b ? 1 : 0);
        esp_rmaker_param_update(param, val);

    } else {
        ESP_LOGW(TAG, "Unknown param: %s / %s", dev_name, param_name);
    }
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════
 *  event_handler — RainMaker 生命周期事件
 * ═══════════════════════════════════════════════════════ */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == RMAKER_EVENT) {
        switch (event_id) {
            case RMAKER_EVENT_INIT_DONE:
                ESP_LOGI(TAG, "RainMaker Initialised.");
                break;
            case RMAKER_EVENT_CLAIM_STARTED:
                ESP_LOGI(TAG, "RainMaker Claim Started.");
                break;
            case RMAKER_EVENT_CLAIM_SUCCESSFUL:
                ESP_LOGI(TAG, "RainMaker Claim Successful.");
                break;
            case RMAKER_EVENT_CLAIM_FAILED:
                ESP_LOGI(TAG, "RainMaker Claim Failed.");
                break;
            default: break;
        }
    } else if (event_base == RMAKER_COMMON_EVENT) {
        switch (event_id) {
            case RMAKER_EVENT_REBOOT:
                ESP_LOGI(TAG, "Rebooting in %d seconds.", *((uint8_t *)event_data));
                break;
            case RMAKER_EVENT_WIFI_RESET:
                ESP_LOGI(TAG, "Wi-Fi credentials reset.");
                break;
            case RMAKER_EVENT_FACTORY_RESET:
                ESP_LOGI(TAG, "Node reset to factory defaults.");
                break;
            case RMAKER_MQTT_EVENT_CONNECTED:
                ESP_LOGI(TAG, "MQTT Connected.");
                break;
            case RMAKER_MQTT_EVENT_DISCONNECTED:
                ESP_LOGI(TAG, "MQTT Disconnected.");
                break;
            case RMAKER_MQTT_EVENT_PUBLISHED:
                ESP_LOGI(TAG, "MQTT Published. Msg id: %d.", *((int *)event_data));
                break;
            default: break;
        }
    } else if (event_base == APP_NETWORK_EVENT) {
        switch (event_id) {
            case APP_NETWORK_EVENT_QR_DISPLAY:
                ESP_LOGI(TAG, "Provisioning QR: %s", (char *)event_data);
                break;
            case APP_NETWORK_EVENT_PROV_TIMEOUT:
                ESP_LOGI(TAG, "Provisioning Timed Out. Please reboot.");
                break;
            case APP_NETWORK_EVENT_PROV_RESTART:
                ESP_LOGI(TAG, "Provisioning restarted due to failures.");
                break;
            default: break;
        }
    } else if (event_base == RMAKER_OTA_EVENT) {
        switch (event_id) {
            case RMAKER_OTA_EVENT_STARTING:
                ESP_LOGI(TAG, "Starting OTA.");
                break;
            case RMAKER_OTA_EVENT_IN_PROGRESS:
                ESP_LOGI(TAG, "OTA in progress.");
                break;
            case RMAKER_OTA_EVENT_SUCCESSFUL:
                ESP_LOGI(TAG, "OTA successful.");
                break;
            case RMAKER_OTA_EVENT_FAILED:
                ESP_LOGI(TAG, "OTA Failed.");
                break;
            case RMAKER_OTA_EVENT_REQ_FOR_REBOOT:
                ESP_LOGI(TAG, "Firmware downloaded. Please reboot.");
                break;
            default: break;
        }
    }
}

void rainmaker_register_event_handlers(void)
{
    esp_event_handler_register(RMAKER_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
    esp_event_handler_register(RMAKER_COMMON_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
    esp_event_handler_register(APP_NETWORK_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
    esp_event_handler_register(RMAKER_OTA_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
}

/* ═══════════════════════════════════════════════════════
 *  rainmaker_init — 创建节点 + 三个设备
 * ═══════════════════════════════════════════════════════ */
esp_err_t rainmaker_init(void)
{
    esp_rmaker_node_t *node = esp_rmaker_node_init(
        &(esp_rmaker_config_t){.enable_time_sync = false},
        "VirtualGPS Board", "FatigueGuard_VGPS");
    if (!node) {
        ESP_LOGE(TAG, "Node init failed!");
        return ESP_FAIL;
    }

    /* ── 设备 1: GPS State（只读传感器） ──────────────── */
    gps_device = esp_rmaker_device_create("GPS State",
                    ESP_RMAKER_DEVICE_TEMP_SENSOR, NULL);

    esp_rmaker_param_t *speed_p = esp_rmaker_param_create("Speed",
        ESP_RMAKER_PARAM_TEMPERATURE, esp_rmaker_float(0), PROP_FLAG_READ);
    esp_rmaker_device_add_param(gps_device, speed_p);
    esp_rmaker_device_assign_primary_param(gps_device, speed_p);

    esp_rmaker_param_t *loc_p = esp_rmaker_param_create("Location",
        "esp.param.location", esp_rmaker_int(0), PROP_FLAG_READ);
    esp_rmaker_device_add_param(gps_device, loc_p);

    esp_rmaker_param_t *drv_p = esp_rmaker_param_create("Driver ID",
        "esp.param.driver_id", esp_rmaker_int(1), PROP_FLAG_READ);
    esp_rmaker_device_add_param(gps_device, drv_p);

    esp_rmaker_param_t *dur_p = esp_rmaker_param_create("Drive Minutes",
        "esp.param.duration", esp_rmaker_int(0), PROP_FLAG_READ);
    esp_rmaker_device_add_param(gps_device, dur_p);

    esp_rmaker_param_t *lat_p = esp_rmaker_param_create("Latitude",
        "esp.param.latitude", esp_rmaker_float(26.054f), PROP_FLAG_READ);
    esp_rmaker_device_add_param(gps_device, lat_p);

    esp_rmaker_param_t *lng_p = esp_rmaker_param_create("Longitude",
        "esp.param.longitude", esp_rmaker_float(119.243f), PROP_FLAG_READ);
    esp_rmaker_device_add_param(gps_device, lng_p);

    esp_rmaker_param_t *src_p = esp_rmaker_param_create("GPS Source",
        "esp.param.gps_source", esp_rmaker_int(1), PROP_FLAG_READ);
    esp_rmaker_device_add_param(gps_device, src_p);

    esp_rmaker_node_add_device(node, gps_device);

    /* ── 设备 2: Fatigue State（只读传感器） ──────────── */
    fatigue_device = esp_rmaker_device_create("Fatigue",
                        ESP_RMAKER_DEVICE_TEMP_SENSOR, NULL);

    esp_rmaker_param_t *fat_p = esp_rmaker_param_create("Fatigue Level",
        ESP_RMAKER_PARAM_TEMPERATURE, esp_rmaker_int(0), PROP_FLAG_READ);
    esp_rmaker_device_add_param(fatigue_device, fat_p);
    esp_rmaker_device_assign_primary_param(fatigue_device, fat_p);

    esp_rmaker_param_t *face_p = esp_rmaker_param_create("Face Verified",
        "esp.param.face", esp_rmaker_bool(false), PROP_FLAG_READ);
    esp_rmaker_device_add_param(fatigue_device, face_p);

    esp_rmaker_param_t *alc_p = esp_rmaker_param_create("Alcohol",
        "esp.param.alcohol", esp_rmaker_bool(false), PROP_FLAG_READ);
    esp_rmaker_device_add_param(fatigue_device, alc_p);

    esp_rmaker_param_t *drvc_p = esp_rmaker_param_create("Driver Confirmed",
        "esp.param.driver_confirmed", esp_rmaker_int(0), PROP_FLAG_READ);
    esp_rmaker_device_add_param(fatigue_device, drvc_p);

    esp_rmaker_param_t *sys_p = esp_rmaker_param_create("System Status",
        "esp.param.sys_status", esp_rmaker_int(0), PROP_FLAG_READ);
    esp_rmaker_device_add_param(fatigue_device, sys_p);

    esp_rmaker_node_add_device(node, fatigue_device);

    /* ── 设备 3: Control（读写，远程控制） ─────────────── */
    control_device = esp_rmaker_device_create("Control",
                        ESP_RMAKER_DEVICE_SWITCH, NULL);
    esp_rmaker_device_add_cb(control_device, write_cb, NULL);

    esp_rmaker_param_t *voice_p = esp_rmaker_param_create("Voice Alert",
        ESP_RMAKER_PARAM_MODE, esp_rmaker_int(0),
        PROP_FLAG_READ | PROP_FLAG_WRITE);
    if (voice_p) {
        esp_rmaker_param_add_ui_type(voice_p, ESP_RMAKER_UI_DROPDOWN);
        esp_rmaker_param_add_bounds(voice_p, esp_rmaker_int(0),
                                     esp_rmaker_int(SYN_VOICE_COUNT - 1),
                                     esp_rmaker_int(1));
        esp_rmaker_device_add_param(control_device, voice_p);
    }

    esp_rmaker_param_t *switch_p = esp_rmaker_param_create("Driver Switch",
        "esp.param.driver_switch", esp_rmaker_bool(false),
        PROP_FLAG_READ | PROP_FLAG_WRITE);
    if (switch_p) {
        esp_rmaker_param_add_ui_type(switch_p, ESP_RMAKER_UI_TOGGLE);
        esp_rmaker_device_add_param(control_device, switch_p);
    }

    /* ── 新增: Speed 滑动条 (0-150 km/h, step 5) ────────── */
    esp_rmaker_param_t *ctrl_speed_p = esp_rmaker_param_create("Speed",
        ESP_RMAKER_PARAM_TEMPERATURE, esp_rmaker_int(0),
        PROP_FLAG_READ | PROP_FLAG_WRITE);
    if (ctrl_speed_p) {
        esp_rmaker_param_add_ui_type(ctrl_speed_p, ESP_RMAKER_UI_SLIDER);
        esp_rmaker_param_add_bounds(ctrl_speed_p, esp_rmaker_int(0),
                                     esp_rmaker_int(150), esp_rmaker_int(5));
        esp_rmaker_device_add_param(control_device, ctrl_speed_p);
    }

    /* ── 新增: Location 下拉 (普通/服务区/应急) ─────────── */
    esp_rmaker_param_t *ctrl_loc_p = esp_rmaker_param_create("Location",
        "esp.param.location", esp_rmaker_int(0),
        PROP_FLAG_READ | PROP_FLAG_WRITE);
    if (ctrl_loc_p) {
        esp_rmaker_param_add_ui_type(ctrl_loc_p, ESP_RMAKER_UI_DROPDOWN);
        esp_rmaker_param_add_bounds(ctrl_loc_p, esp_rmaker_int(0),
                                     esp_rmaker_int(2), esp_rmaker_int(1));
        esp_rmaker_device_add_param(control_device, ctrl_loc_p);
    }

    /* ── 新增: Weather Risk 下拉 ────────────────────────── */
    esp_rmaker_param_t *ctrl_weather_p = esp_rmaker_param_create("Weather Risk",
        "esp.param.weather_risk", esp_rmaker_int(0),
        PROP_FLAG_READ | PROP_FLAG_WRITE);
    if (ctrl_weather_p) {
        esp_rmaker_param_add_ui_type(ctrl_weather_p, ESP_RMAKER_UI_DROPDOWN);
        esp_rmaker_param_add_bounds(ctrl_weather_p, esp_rmaker_int(0),
                                     esp_rmaker_int(2), esp_rmaker_int(1));
        esp_rmaker_device_add_param(control_device, ctrl_weather_p);
    }

    /* ── 新增: Fatigue Level 下拉 ───────────────────────── */
    esp_rmaker_param_t *ctrl_fat_p = esp_rmaker_param_create("Fatigue Level",
        ESP_RMAKER_PARAM_TEMPERATURE, esp_rmaker_int(0),
        PROP_FLAG_READ | PROP_FLAG_WRITE);
    if (ctrl_fat_p) {
        esp_rmaker_param_add_ui_type(ctrl_fat_p, ESP_RMAKER_UI_DROPDOWN);
        esp_rmaker_param_add_bounds(ctrl_fat_p, esp_rmaker_int(0),
                                     esp_rmaker_int(2), esp_rmaker_int(1));
        esp_rmaker_device_add_param(control_device, ctrl_fat_p);
    }

    /* ── 新增: PERCLOS 滑动条 (0-100%) ──────────────────── */
    esp_rmaker_param_t *ctrl_perclos_p = esp_rmaker_param_create("PERCLOS",
        "esp.param.perclos", esp_rmaker_int(0),
        PROP_FLAG_READ | PROP_FLAG_WRITE);
    if (ctrl_perclos_p) {
        esp_rmaker_param_add_ui_type(ctrl_perclos_p, ESP_RMAKER_UI_SLIDER);
        esp_rmaker_param_add_bounds(ctrl_perclos_p, esp_rmaker_int(0),
                                     esp_rmaker_int(100), esp_rmaker_int(1));
        esp_rmaker_device_add_param(control_device, ctrl_perclos_p);
    }

    /* ── 新增: Alcohol 开关 ─────────────────────────────── */
    esp_rmaker_param_t *ctrl_alc_p = esp_rmaker_param_create("Alcohol",
        "esp.param.alcohol", esp_rmaker_bool(false),
        PROP_FLAG_READ | PROP_FLAG_WRITE);
    if (ctrl_alc_p) {
        esp_rmaker_param_add_ui_type(ctrl_alc_p, ESP_RMAKER_UI_TOGGLE);
        esp_rmaker_device_add_param(control_device, ctrl_alc_p);
    }

    /* ── 新增: Face Verified 开关 ───────────────────────── */
    esp_rmaker_param_t *ctrl_face_p = esp_rmaker_param_create("Face Verified",
        "esp.param.face", esp_rmaker_bool(false),
        PROP_FLAG_READ | PROP_FLAG_WRITE);
    if (ctrl_face_p) {
        esp_rmaker_param_add_ui_type(ctrl_face_p, ESP_RMAKER_UI_TOGGLE);
        esp_rmaker_device_add_param(control_device, ctrl_face_p);
    }

    /* ── 新增: +30 Min 开关 ─────────────────────────────── */
    esp_rmaker_param_t *ctrl_drive_p = esp_rmaker_param_create("+30 Min",
        "esp.param.drive_plus", esp_rmaker_bool(false),
        PROP_FLAG_READ | PROP_FLAG_WRITE);
    if (ctrl_drive_p) {
        esp_rmaker_param_add_ui_type(ctrl_drive_p, ESP_RMAKER_UI_TOGGLE);
        esp_rmaker_device_add_param(control_device, ctrl_drive_p);
    }

    /* ── 新增: Rest Request 开关 ────────────────────────── */
    esp_rmaker_param_t *ctrl_rest_p = esp_rmaker_param_create("Rest Request",
        "esp.param.rest_request", esp_rmaker_bool(false),
        PROP_FLAG_READ | PROP_FLAG_WRITE);
    if (ctrl_rest_p) {
        esp_rmaker_param_add_ui_type(ctrl_rest_p, ESP_RMAKER_UI_TOGGLE);
        esp_rmaker_device_add_param(control_device, ctrl_rest_p);
    }

    /* ── 新增: Demo Mode 开关 (触发主板疲劳预测演示, 同酒精格式) ── */
    esp_rmaker_param_t *ctrl_demo_p = esp_rmaker_param_create("Demo Mode",
        "esp.param.demo", esp_rmaker_bool(false),
        PROP_FLAG_READ | PROP_FLAG_WRITE);
    if (ctrl_demo_p) {
        esp_rmaker_param_add_ui_type(ctrl_demo_p, ESP_RMAKER_UI_TOGGLE);
        esp_rmaker_device_add_param(control_device, ctrl_demo_p);
    }

    /* ── 新增: 恢复真实疲劳检测开关 (清除 PERCLOS mock 覆盖) ── */
    esp_rmaker_param_t *ctrl_reset_p = esp_rmaker_param_create("Reset Perclos",
        "esp.param.reset_perclos", esp_rmaker_bool(false),
        PROP_FLAG_READ | PROP_FLAG_WRITE);
    if (ctrl_reset_p) {
        esp_rmaker_param_add_ui_type(ctrl_reset_p, ESP_RMAKER_UI_TOGGLE);
        esp_rmaker_device_add_param(control_device, ctrl_reset_p);
    }

    esp_rmaker_device_assign_primary_param(control_device, voice_p);

    esp_rmaker_node_add_device(node, control_device);

    ESP_LOGI(TAG, "Devices created: GPS State + Fatigue + Control");
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════
 *  rainmaker_report_gps_state
 * ═══════════════════════════════════════════════════════ */
void rainmaker_report_gps_state(float speed_kmh, int location,
                                int driver_id, int drive_seconds,
                                float lat, float lng, bool use_real_gps)
{
    if (!gps_device) return;

    if (speed_kmh != g_last_speed) {
        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_name(gps_device, "Speed"),
            esp_rmaker_float(speed_kmh));
        g_last_speed = speed_kmh;
    }
    if (location != g_last_location) {
        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_name(gps_device, "Location"),
            esp_rmaker_int(location));
        g_last_location = location;
    }
    if (driver_id != g_last_driver_id) {
        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_name(gps_device, "Driver ID"),
            esp_rmaker_int(driver_id));
        g_last_driver_id = driver_id;
    }
    int drive_min = drive_seconds / 60;
    if (drive_min != g_last_drive_min) {
        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_name(gps_device, "Drive Minutes"),
            esp_rmaker_int(drive_min));
        g_last_drive_min = drive_min;
    }
    /* lat/lng/gps source: only report when actually changed */
    if (lat != g_last_lat) {
        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_name(gps_device, "Latitude"),
            esp_rmaker_float(lat));
        g_last_lat = lat;
    }
    if (lng != g_last_lng) {
        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_name(gps_device, "Longitude"),
            esp_rmaker_float(lng));
        g_last_lng = lng;
    }
    int gps_src = use_real_gps ? 0 : 1;
    if (gps_src != g_last_gps_src) {
        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_name(gps_device, "GPS Source"),
            esp_rmaker_int(gps_src));
        g_last_gps_src = gps_src;
    }
}

/* ═══════════════════════════════════════════════════════
 *  rainmaker_report_fatigue_state
 * ═══════════════════════════════════════════════════════ */
void rainmaker_report_fatigue_state(int fatigue_level, int face_verified,
                                    int alcohol, int driver_confirmed,
                                    int sys_status)
{
    if (!fatigue_device) return;

    if (fatigue_level != g_last_fatigue) {
        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_name(fatigue_device, "Fatigue Level"),
            esp_rmaker_int(fatigue_level));
        g_last_fatigue = fatigue_level;
    }
    if (face_verified != g_last_face) {
        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_name(fatigue_device, "Face Verified"),
            esp_rmaker_bool(face_verified != 0));
        g_last_face = face_verified;
    }
    if (alcohol != g_last_alcohol) {
        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_name(fatigue_device, "Alcohol"),
            esp_rmaker_bool(alcohol != 0));
        g_last_alcohol = alcohol;
    }
    if (driver_confirmed != g_last_drv_conf) {
        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_name(fatigue_device, "Driver Confirmed"),
            esp_rmaker_int(driver_confirmed));
        g_last_drv_conf = driver_confirmed;
    }
    if (sys_status != g_last_sys) {
        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_name(fatigue_device, "System Status"),
            esp_rmaker_int(sys_status));
        g_last_sys = sys_status;
    }
}

/* ═══════════════════════════════════════════════════════
 *  rainmaker_start_periodic_report
 * ═══════════════════════════════════════════════════════ */
static void periodic_report_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));  /* 30s, 避免触发 MQTT budget */

        virtual_gps_state_t state;
        gps_state_read(&state);

        rainmaker_report_gps_state(
            state.speed_kmh, (int)state.location,
            state.driver_id, state.drive_seconds,
            state.gps_lat, state.gps_lng, state.use_real_gps);
    }
}

void rainmaker_start_periodic_report(void)
{
    xTaskCreate(periodic_report_task, "rm_gps_rpt", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "Periodic GPS report task started (30s)");
}
