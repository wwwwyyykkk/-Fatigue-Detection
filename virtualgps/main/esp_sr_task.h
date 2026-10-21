/**
 * esp_sr_task.h — VirtualGPS ESP-SR 语音接口 (INMP441 I2S 麦克风)
 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化 ESP-SR (加载模型 + 启动 I2S 采集/识别任务) */
bool esp_sr_init(void);

#ifdef __cplusplus
}
#endif
