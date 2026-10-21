/**
 * alcohol_sensor.c - MQ3 酒精传感器驱动实现
 *
 * 检测策略:
 *   1. DO 引脚 (GPIO6): LOW = 超过阈值 → 立即触发
 *   2. ADC (GPIO2/ADC1_CH1): raw > MQ3_ADC_THRESHOLD → 触发
 *   3. 冷却 30 秒不重复播报
 */

#include "alcohol_sensor.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/adc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MQ3";

static int64_t s_init_us = 0;
static int64_t s_last_alert_us = 0;
static bool    s_is_alerting = false;

void alcohol_sensor_init(void)
{
    /* ADC: GPIO2 = ADC1_CH1 */
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(MQ3_ADC_CHANNEL, MQ3_ADC_ATTEN);

    /* DO: GPIO6, 内部上拉 (MQ3 DO 为开漏输出，未触发时高阻，触发拉低) */
    gpio_config_t do_cfg = {
        .pin_bit_mask = (1ULL << MQ3_DO_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&do_cfg);

    s_init_us = esp_timer_get_time();

    ESP_LOGI(TAG, "MQ3 init: ADC=GPIO2(ADC1_CH1) DO=GPIO6 thresh=%d cooldown=%dms",
             MQ3_ADC_THRESHOLD, MQ3_COOLDOWN_MS);
}

bool alcohol_sensor_check(void)
{
    int64_t now = esp_timer_get_time();

    /* ── MQ3 预热期 60 秒，跳过检测 ── */
    if ((now - s_init_us) / 1000000 < 60) {
        return false;
    }

    /* ── 检查冷却时间 ── */
    if (s_is_alerting && (now - s_last_alert_us) / 1000 < MQ3_COOLDOWN_MS) {
        return false;
    }
    s_is_alerting = false;

    /* ── 1. DO 引脚检测 (LOW = 酒精超标, 有上拉悬空为HIGH不误触) ── */
    int do_level = gpio_get_level(MQ3_DO_PIN);

    /* ── 2. ADC 多采样取平均 ── */
    int sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += adc1_get_raw(MQ3_ADC_CHANNEL);
    }
    int raw = sum / 16;

    /* DO=LOW 且 ADC超阈值 才触发（避免悬空误报） */
    if (do_level == 0 && raw > MQ3_ADC_THRESHOLD) {
        float voltage = raw * 3.1f / 4095.0f;
        ESP_LOGW(TAG, "ALCOHOL DETECTED! DO=0 ADC=%d (%.2fV) > thresh=%d",
                 raw, voltage, MQ3_ADC_THRESHOLD);
        s_is_alerting = true;
        s_last_alert_us = now;
        return true;
    }

    /* 每 10 秒打印一次正常读数 */
    static int64_t last_log_us = 0;
    if (now - last_log_us > 10000000) {
        float voltage = raw * 3.1f / 4095.0f;
        ESP_LOGI(TAG, "Normal: DO=%d ADC=%d (%.2fV)", do_level, raw, voltage);
        last_log_us = now;
    }

    return false;
}

bool alcohol_sensor_in_cooldown(void)
{
    return s_is_alerting;
}
