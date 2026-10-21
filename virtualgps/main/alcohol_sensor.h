/**
 * alcohol_sensor.h - MQ3 酒精传感器驱动
 *
 * 硬件:
 *   ADC: GPIO2 (ADC1_CH1) — 模拟输出，电压随酒精浓度升高
 *   DO:  GPIO6           — 数字输出，LOW=检测到酒精(超过电位器阈值)
 *
 * 检测到酒精时触发语音告警（冷却 30 秒防止重复播报）
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化 MQ3 传感器 (ADC + DO GPIO) */
void alcohol_sensor_init(void);

/**
 * @brief 扫描传感器状态（在主循环中周期性调用）
 * @return true 本轮检测到酒精
 */
bool alcohol_sensor_check(void);

/** @brief 是否正在告警冷却中 */
bool alcohol_sensor_in_cooldown(void);

#ifdef __cplusplus
}
#endif
