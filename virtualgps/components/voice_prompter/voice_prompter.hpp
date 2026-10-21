/**
 * voice_prompter.hpp — I2S 语音播报封装（MAX98357）
 *
 * 基于 ESP-IDF I2S 驱动，通过 MAX98357 I2S 功放驱动喇叭。
 * WAV 文件要求 8000Hz/16bit/mono（已预处理）。
 *
 * 接线（VirtualGPS 板）：
 *   MAX98357 BCK → GPIO18 (I2S_BCK)
 *   MAX98357 LRC → GPIO19 (I2S_LRC/WS)
 *   MAX98357 DIN → GPIO20 (I2S_DIN)
 *   MAX98357 VIN → 5V
 *   MAX98357 GND → GND
 *   喇叭接 MAX98357 OUT+ / OUT-
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 播放完成回调 */
typedef void (*voice_complete_cb_t)(void *user_ctx);

/**
 * @brief 初始化 I2S + MAX98357 语音播报
 * @return true 成功
 */
bool voice_prompter_init(void);

/**
 * @brief 播放一段 WAV 音频（非阻塞）
 * @param wav_data  WAV 文件完整数据
 * @param wav_len   数据长度
 * @param cb        完成回调（可为 NULL）
 * @param user_ctx  回调上下文
 * @return true 播放开始
 */
bool voice_prompter_play(const uint8_t *wav_data, size_t wav_len,
                         voice_complete_cb_t cb, void *user_ctx);

/** @brief 是否正在播放 */
bool voice_prompter_is_playing(void);

/** @brief 停止播放 */
void voice_prompter_stop(void);

/** @brief 设置音量 0-100（通过 I2S 增益调节） */
void voice_prompter_set_volume(uint8_t percent);

/** @brief 反初始化 */
void voice_prompter_deinit(void);

#ifdef __cplusplus
}
#endif
