/**
 * voice_prompter.cpp — I2S 语音播报实现（MAX98357）
 *
 * 使用 ESP-IDF I2S 驱动，播放 8000Hz/16bit/mono WAV 音频。
 * 在独立任务中 DMA 传输，不阻塞主循环。
 */
#include "voice_prompter.hpp"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "Voice";

/* ─── 硬件引脚配置（VirtualGPS 板：GPIO18/19/20） ───────── */
#define I2S_BCK_PIN     GPIO_NUM_18
#define I2S_LRC_PIN     GPIO_NUM_19   /* WS */
#define I2S_DIN_PIN     GPIO_NUM_20   /* DATA */

#define SAMPLE_RATE     8000
#define BITS_PER_SAMPLE 16
#define DMA_BUF_COUNT   4
#define DMA_BUF_LEN     512   /* 每块 512 个采样 */

/* ─── 状态 ──────────────────────────────────────────────── */
static SemaphoreHandle_t       s_mutex = NULL;
static volatile bool           s_playing = false;
static const uint8_t          *s_pcm_ptr = NULL;
static size_t                  s_pcm_remain = 0;
static voice_complete_cb_t     s_cb = NULL;
static void                   *s_cb_ctx = NULL;
static TaskHandle_t            s_play_task = NULL;
static i2s_chan_handle_t       s_tx_chan = NULL;
static float                   s_volume = 1.0f;  /* 0.0 ~ 1.0 */

/* ─── WAV 解析 ──────────────────────────────────────────── */
static bool parse_wav(const uint8_t *wav, size_t wav_len,
                      const uint8_t **pcm, size_t *pcm_len)
{
    if (!wav || wav_len < 44) return false;
    const uint32_t riff = *(const uint32_t *)(wav + 0);
    const uint32_t wave = *(const uint32_t *)(wav + 8);
    if (riff != 0x46464952 || wave != 0x45564157) return false;

    const uint8_t *p = wav + 12;
    while (p + 8 <= wav + wav_len) {
        uint32_t id = *(const uint32_t *)p;
        uint32_t sz = *(const uint32_t *)(p + 4);
        if (id == 0x61746164) {  /* "data" */
            *pcm = p + 8;
            *pcm_len = sz;
            return true;
        }
        p += 8 + sz;
    }
    return false;
}

/* ─── I2S 初始化 ─────────────────────────────────────────── */
static bool i2s_init(void)
{
    if (s_tx_chan) return true;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = DMA_BUF_LEN;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel: %s", esp_err_to_name(err));
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_PIN,
            .ws   = I2S_LRC_PIN,
            .dout = I2S_DIN_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    std_cfg.slot_cfg.bit_shift = true;

    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S std init: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx_chan); s_tx_chan = NULL;
        return false;
    }

    err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S enable: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx_chan); s_tx_chan = NULL;
        return false;
    }

    ESP_LOGI(TAG, "I2S ready: BCK=%d LRC=%d DIN=%d %dHz 16bit mono",
             I2S_BCK_PIN, I2S_LRC_PIN, I2S_DIN_PIN, SAMPLE_RATE);
    return true;
}

/* ─── 播放任务 ──────────────────────────────────────────── */
static void playback_task(void *arg)
{
    (void)arg;
    int16_t *buf = (int16_t *)malloc(DMA_BUF_LEN * sizeof(int16_t));
    if (!buf) {
        ESP_LOGE(TAG, "OOM for DMA buffer");
        s_playing = false; s_play_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (s_playing && s_pcm_remain >= 2) {
        size_t frames = s_pcm_remain / 2;
        if (frames > DMA_BUF_LEN) frames = DMA_BUF_LEN;

        if (s_volume < 0.99f || s_volume > 1.01f) {
            for (size_t i = 0; i < frames; i++) {
                int16_t sample = ((const int16_t *)s_pcm_ptr)[i];
                buf[i] = (int16_t)(sample * s_volume);
            }
        } else {
            memcpy(buf, s_pcm_ptr, frames * 2);
        }

        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, buf, frames * 2,
                                          &bytes_written, pdMS_TO_TICKS(200));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2S write: %s", esp_err_to_name(err));
            break;
        }
        s_pcm_ptr += frames * 2;
        s_pcm_remain -= frames * 2;
    }

    free(buf);
    ESP_LOGI(TAG, "Playback finished");
    s_playing = false;
    if (s_cb) s_cb(s_cb_ctx);
    s_play_task = NULL;
    vTaskDelete(NULL);
}

/* ══════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════ */

bool voice_prompter_init(void)
{
    if (s_tx_chan) return true;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return false;
    if (!i2s_init()) {
        vSemaphoreDelete(s_mutex); s_mutex = NULL;
        return false;
    }
    ESP_LOGI(TAG, "Voice prompter ready (I2S + MAX98357)");
    return true;
}

bool voice_prompter_play(const uint8_t *wav_data, size_t wav_len,
                         voice_complete_cb_t cb, void *user_ctx)
{
    if (!s_mutex || !wav_data || wav_len < 44) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_playing) { s_playing = false; vTaskDelay(pdMS_TO_TICKS(50)); }

    const uint8_t *pcm = NULL; size_t pcm_len = 0;
    if (!parse_wav(wav_data, wav_len, &pcm, &pcm_len)) {
        xSemaphoreGive(s_mutex); return false;
    }

    s_pcm_ptr = pcm; s_pcm_remain = pcm_len;
    s_cb = cb; s_cb_ctx = user_ctx;
    s_playing = true;

    BaseType_t ok = xTaskCreatePinnedToCore(
        playback_task, "voice_play", 4096, NULL, 8, &s_play_task, tskNO_AFFINITY);
    if (ok != pdTRUE) { s_playing = false; xSemaphoreGive(s_mutex); return false; }

    ESP_LOGI(TAG, "Playing %zu PCM bytes (%.1f sec)",
             pcm_len, (float)pcm_len / 2.0f / SAMPLE_RATE);
    xSemaphoreGive(s_mutex);
    return true;
}

bool voice_prompter_is_playing(void) { return s_playing; }

void voice_prompter_stop(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_playing = false;
    if (s_play_task) vTaskDelay(pdMS_TO_TICKS(50));
    xSemaphoreGive(s_mutex);
}

void voice_prompter_set_volume(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_volume = percent / 100.0f;
}

void voice_prompter_deinit(void)
{
    voice_prompter_stop();
    if (s_tx_chan) {
        i2s_channel_disable(s_tx_chan);
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
    }
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
}
