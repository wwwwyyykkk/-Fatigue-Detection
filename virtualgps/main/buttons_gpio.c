/**
 * buttons_gpio.c - ADC按键驱动实现 (GPIO1=ADC1_CH0)
 *
 * 单ADC引脚 + 6个电阻分压按键
 * R1=1k R2=2k R3=3.9k R4=8.2k R5=16k R6=33k
 * 电路: GPIO1内部上拉 → 按键+电阻→GND
 */

#include "buttons_gpio.h"
#include "gps_state.h"
#include "config.h"
#include "uart_protocol.h"
#include "app_priv.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/adc.h"

/* 语音播报 (SYN6288 TTS) */
#include "syn6288.h"

/* ── 上报 GPS 状态到 RainMaker ─────────────────────────── */
static void report_to_rainmaker(void)
{
    virtual_gps_state_t state;
    gps_state_read(&state);
    rainmaker_report_gps_state(state.speed_kmh, (int)state.location,
                                state.driver_id, state.drive_seconds,
                                state.gps_lat, state.gps_lng, state.use_real_gps);
}

static const char *TAG = "BUTTONS";

/* ========== ADC信号 → 按键ID ========== */
static button_id_t adc_to_button(int raw)
{
    if (raw <= ADC_THR_BTN1_MAX) return BUTTON_1;
    if (raw <= ADC_THR_BTN2_MAX) return BUTTON_2;
    if (raw <= ADC_THR_BTN3_MAX) return BUTTON_3;
    if (raw <= ADC_THR_BTN4_MAX) return BUTTON_4;
    if (raw <= ADC_THR_BTN5_MAX) return BUTTON_5;
    if (raw <= ADC_THR_BTN6_MAX) return BUTTON_6;
    return BUTTON_NONE;  // >4090 无按键
}

/* ========== 初始化ADC ========== */
void buttons_init(void)
{
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC_BUTTON_CHANNEL, ADC_BUTTON_ATTEN);

    // 内部上拉 + 外部上拉双重保障, 避免浮空导致ADC抖动
    gpio_set_pull_mode(GPIO_NUM_1, GPIO_PULLUP_ONLY);

    ESP_LOGI(TAG, "ADC buttons initialized (GPIO1, int+ext pull-up, 5-key)");
    ESP_LOGI(TAG, "  BTN1=1k BTN2=2k BTN3=弃用 BTN4=8.2k BTN5=16k BTN6=33k");
}

/* ========== 扫描按键 (返回按下的第一个按键ID) ========== */
button_id_t buttons_scan(void)
{
    // 多次采样取平均
    int sum = 0;
    for (int i = 0; i < 32; i++) {
        sum += adc1_get_raw(ADC_BUTTON_CHANNEL);
    }
    int raw = sum / 32;

    button_id_t btn = adc_to_button(raw);

    // 只在按键变化时打印 (避免刷屏)
    static int last_raw = -1;
    static button_id_t last_btn = BUTTON_NONE;
    if (btn != last_btn || abs(raw - last_raw) > 50) {
        float voltage = raw * 3.1f / 4095.0f;
        ESP_LOGI(TAG, "ADC raw=%4d  V=%.2fV  → btn=%d", raw, voltage, (int)btn);
        last_raw = raw;
        last_btn = btn;
    }

    return btn;
}

/*
 * 速度循环：
 * 第一次按 BUTTON_1 -> 0 km/h
 * 第二次按 BUTTON_1 -> 80 km/h
 * 第三次按 BUTTON_1 -> 150 km/h
 * 再按回到 0 km/h
 */
static int s_speed_index = -1;
static const float s_speed_table[] = { 0.0f, 80.0f, 150.0f };
#define SPEED_COUNT (sizeof(s_speed_table) / sizeof(s_speed_table[0]))

/* ========== 按键处理逻辑 ========== */
static void handle_button_press(button_id_t btn)
{
    switch (btn) {
        /* BUTTON_1: 速度循环 0→80→150→0 */
        case BUTTON_1: {
            s_speed_index++;
            if (s_speed_index >= SPEED_COUNT) s_speed_index = 0;
            float speed = s_speed_table[s_speed_index];
            ESP_LOGI(TAG, "Button 1 -> Speed cycle: %.0f km/h", speed);
            gps_state_set_speed(speed);
            if (speed >= 150.0f) syn6288_speak(SYN_VOICE_OVERSPEED);
            uart_send_now();
            report_to_rainmaker();
            break;
        }
        /* BUTTON_2: 地点循环 普通→服务区→应急 */
        case BUTTON_2: {
            ESP_LOGI(TAG, "Button 2 -> Cycle location");
            int new_loc = gps_state_cycle_location();
            if (new_loc == 1) syn6288_speak(SYN_VOICE_SERVICE_AREA);
            else if (new_loc == 2) syn6288_speak(SYN_VOICE_EMERGENCY_LANE);
            uart_send_now();
            report_to_rainmaker();
            break;
        }
        /* BUTTON_3: 休息请求 — 本地检查速度==0 且 位置==服务区 */
        case BUTTON_3:
            buttons_handle_rest_request();
            break;
        /* BUTTON_4: 真实/虚拟GPS切换 (原BTN3功能移到此) */
        case BUTTON_4: {
            bool now_real = gps_state_toggle_gps_source();
            ESP_LOGI(TAG, "Button 4 -> GPS source: %s", now_real ? "REAL" : "VIRTUAL");
            uart_send_now();
            report_to_rainmaker();
            break;
        }
        /* BUTTON_5: 司机切换 */
        case BUTTON_5:
            ESP_LOGI(TAG, "Button 5 -> Toggle driver");
            gps_state_toggle_driver();
            syn6288_speak(SYN_VOICE_DRIVER_SWITCH);
            syn6288_speak(SYN_VOICE_IDENTIFY_START);
            uart_send_now();
            report_to_rainmaker();
            break;
        /* BUTTON_6: 驾驶时长 +30min */
        case BUTTON_6: {
            ESP_LOGI(TAG, "Button 6 -> Add 30min drive time");
            int total_min = gps_state_add_drive_time(30);
            if (total_min >= 240) syn6288_speak(SYN_VOICE_DRIVE_4H);
            else if (total_min >= 210) syn6288_speak(SYN_VOICE_DRIVE_3_5H);
            else if (total_min >= 180) syn6288_speak(SYN_VOICE_DRIVE_3H);
            uart_send_now();
            report_to_rainmaker();
            break;
        }
        default:
            break;
    }
}

/* ========== 休息请求 (公开接口，物理按键 & Rainmaker 复用) ========== */
void buttons_handle_rest_request(void)
{
    /* Toggle: if already in rest → request stop; else → request start */
    if (g_in_rest) {
        ESP_LOGI(TAG, "Rest stop request (toggle)");
        uart_send_rest_stop();
        g_in_rest = false;  /* immediately update local state */
        report_to_rainmaker();
        return;
    }

    ESP_LOGI(TAG, "Rest start request (API call)");
    virtual_gps_state_t st;
    gps_state_read(&st);

    if (st.speed_kmh > 0.0f) {
        ESP_LOGW(TAG, "REST DENIED: speed=%.1f km/h", st.speed_kmh);
        syn6288_speak(SYN_VOICE_NEED_STOP);
    } else if (st.location != LOCATION_SERVICE_AREA) {
        ESP_LOGW(TAG, "REST DENIED: location=%d", (int)st.location);
        if (st.location == LOCATION_EMERGENCY_LANE)
            syn6288_speak(SYN_VOICE_EMERGENCY_LANE);
        else
            syn6288_speak(SYN_VOICE_NEED_SERVICE);
    } else {
        ESP_LOGI(TAG, "REST OK: send request to main board");
        uart_send_rest_request();                    /* req=1: start rest */
        syn6288_speak(SYN_VOICE_REST_START);
    }
    report_to_rainmaker();
}

/* ========== 按键扫描任务 (改进去抖: 独立按压/释放检测) ========== */
static void button_scan_task(void *arg)
{
    ESP_LOGI(TAG, "Button scan task started (ADC mode)");

    button_id_t confirmed_btn = BUTTON_NONE;  // 已确认的当前按键
    int press_cnt = 0;    // 连续相同按键次数 (按压确认)
    int release_cnt = 0;  // 连续NONE次数 (释放确认)
    bool fired = false;   // 当前按键是否已触发过

    while (1) {
        button_id_t btn = buttons_scan();

        if (btn != BUTTON_NONE) {
            // ── 有按键按下 ──
            release_cnt = 0;

            if (btn == confirmed_btn) {
                // 相同按键: 累加确认计数
                if (press_cnt < 10) press_cnt++;
            } else {
                // 不同按键或刚按下: 重新开始计数
                confirmed_btn = btn;
                press_cnt = 1;
                fired = false;
            }

            // 连续8次确认 (~240ms) 且未触发过 → 触发一次
            if (press_cnt >= 8 && !fired) {
                fired = true;
                handle_button_press(btn);
            }
        } else {
            // ── 无按键 ──
            press_cnt = 0;

            if (release_cnt < 20) release_cnt++;

            // 连续10次NONE (~300ms) → 完全释放, 允许下次触发
            if (release_cnt >= 10) {
                confirmed_btn = BUTTON_NONE;
                fired = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_SCAN_INTERVAL_MS));
    }
}

/* ========== 启动按键扫描任务 ========== */
void buttons_start_scan_task(void)
{
    xTaskCreate(button_scan_task, "button_scan", 4096, NULL, 5, NULL);
}
