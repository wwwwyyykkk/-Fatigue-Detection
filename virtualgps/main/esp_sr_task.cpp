/**
 * esp_sr_task.cpp — ESP-SR 语音唤醒 + 命令识别 (VirtualGPS)
 * INMP441 I2S 麦克风 (16kHz) → WakeNet 唤醒"你好小智" → MultiNet 命令
 *
 * 与主板区别：采集源从 USB UAC 改为 INMP441 I2S，直接 16kHz 无需升采样。
 * 2026-08-06
 */
#include "esp_sr_task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include <math.h>
#include <string.h>
extern "C" {
#include "uart_protocol.h"
#include "gps_state.h"
#include "syn6288.h"
}

/* esp-sr */
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"

static const char *TAG = "ESPSR";

/* ── 模型句柄 ── */
static const esp_wn_iface_t *s_wn_iface = NULL;
static model_iface_data_t   *s_wn_data  = NULL;
static int s_wn_chunk = 0;

static const esp_mn_iface_t *s_mn_iface = NULL;
static model_iface_data_t   *s_mn_data  = NULL;
static int s_mn_chunk = 0;

/* ── INMP441 I2S 麦克风 (16kHz, 24bit/32slot, mono) ── */
#define I2S_MIC_SCK  GPIO_NUM_8
#define I2S_MIC_WS   GPIO_NUM_9
#define I2S_MIC_SD   GPIO_NUM_11
#define I2S_MIC_RATE 16000

/* ── 音频环形缓冲 (16kHz int16, FIFO, 2 秒) ── */
#define RING_LEN (32000)
static int16_t *s_ring = NULL;      /* PSRAM */
static int s_ring_w = 0;
static int s_ring_count = 0;
static SemaphoreHandle_t s_ring_mutex = NULL;

/* ── I2S 麦克风采集任务：连续读入环形缓冲 ── */
static void mic_capture_task(void *arg)
{
    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&cc, NULL, &rx) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel FAIL"); vTaskDelete(NULL); return;
    }
    i2s_std_config_t sc = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(I2S_MIC_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_MIC_SCK,
            .ws   = I2S_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_MIC_SD,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if (i2s_channel_init_std_mode(rx, &sc) != ESP_OK) {
        ESP_LOGE(TAG, "i2s init FAIL"); vTaskDelete(NULL); return;
    }
    i2s_channel_enable(rx);
    ESP_LOGI(TAG, "I2S mic streaming: %dHz mono (SCK=8 WS=9 SD=11)", I2S_MIC_RATE);

    int32_t *buf = (int32_t *)heap_caps_malloc(1024 * sizeof(int32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGE(TAG, "i2s buf alloc fail"); vTaskDelete(NULL); return; }

    int diag_cnt = 0;
    while (1) {
        size_t bytes = 0;
        if (i2s_channel_read(rx, buf, 1024 * sizeof(int32_t), &bytes, pdMS_TO_TICKS(1000)) != ESP_OK) continue;
        int n = bytes / 4;  /* 32-bit/采样 */
        if (n <= 0) continue;
        /* 诊断: 打印原始样本 (偶数索引=左声道) */
        if (++diag_cnt % 30 == 0) {
            ESP_LOGI(TAG, "DIAG L-ch raw[0..3]=%d %d %d %d",
                     (int)buf[0], (int)buf[2], (int)buf[4], (int)buf[6]);
        }
        xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
        /* 取左声道 (偶数索引): 数据是 stereo 交替, 右声道=0 */
        for (int i = 0; i + 1 < n; i += 2) {
            int32_t v = (buf[i] >> 8) * 40;   /* 24-bit 转 16-bit 并放大 */
            v = v >> 8;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            s_ring[s_ring_w] = (int16_t)v;
            s_ring_w = (s_ring_w + 1) % RING_LEN;
            if (s_ring_count < RING_LEN) s_ring_count++;
        }
        xSemaphoreGive(s_ring_mutex);
    }
}

/* ── 读 n 个最旧样本 (FIFO)，不足返回实际可读数 ── */
static int read_ring(int16_t *dst, int n)
{
    int got = 0;
    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    int readn = (s_ring_count >= n) ? n : 0;
    if (readn > 0) {
        int start = (s_ring_w - s_ring_count + RING_LEN) % RING_LEN;
        for (int i = 0; i < readn; i++) dst[i] = s_ring[(start + i) % RING_LEN];
        s_ring_count -= readn;
        got = readn;
    }
    xSemaphoreGive(s_ring_mutex);
    return got;
}

/* ── 唤醒词/命令处理任务 (16kHz 直采，无需升采样) ── */
static void esp_sr_process_task(void *arg)
{
    int16_t *raw16 = (int16_t *)heap_caps_malloc(1600 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *feed  = (int16_t *)heap_caps_malloc(3200 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw16 || !feed) {
        ESP_LOGE(TAG, "sr buffers alloc fail"); vTaskDelete(NULL); return;
    }
    const int need16 = 1600;  /* 100ms @16kHz */
    int pos = 0;
    bool wake_active = false;

    while (1) {
        if (read_ring(raw16, need16) < need16) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        for (int i = 0; i < need16; i++) {
            feed[pos++] = raw16[i];
            if (wake_active && pos >= s_mn_chunk) {
                esp_mn_state_t ms = s_mn_iface->detect(s_mn_data, feed);
                if (ms == ESP_MN_STATE_DETECTED) {
                    esp_mn_results_t *res = s_mn_iface->get_results(s_mn_data);
                    ESP_LOGI(TAG, ">>> COMMAND: [%s] <<<", res->string);
                    /* ── 命令动作分发 + 语音回复 (2026-08-08) ── */
                    const char *cmd = res->string;
                    if (cmd) {
                        virtual_gps_state_t st;
                        gps_state_read(&st);
                        /* 拼音匹配: "退出休息"(tui chu xiu xi) 含 "xiu xi",先匹配退出的 */
                        if (strstr(cmd, "tui chu") || strstr(cmd, "ji xu")) {
                            uart_send_rest_stop();          /* 退出休息 / 继续驾驶 */
                            syn6288_speak(SYN_VOICE_REST_EXIT);   /* 已退出休息，可以继续驾驶 */
                        } else if (strstr(cmd, "xiu xi") || strstr(cmd, "wo lei")) {
                            /* 休息: 前置条件检查(和主板 start_rest 一致) + 语音反馈 */
                            if (st.speed_kmh > 0.0f) {
                                syn6288_speak(SYN_VOICE_NEED_STOP);     /* 请先停车再休息 */
                            } else if (st.location != 1) {
                                syn6288_speak(SYN_VOICE_NEED_SERVICE);  /* 请到服务区再休息 */
                            } else {
                                uart_send_rest_request();
                                syn6288_speak(SYN_VOICE_REST_START);    /* 已进入休息模式 */
                            }
                        } else if (strstr(cmd, "qie huan")) {
                            gps_state_toggle_driver();
                            syn6288_speak(SYN_VOICE_DRIVER_SWITCH);
                        } else if (strstr(cmd, "cha kan")) {
                            /* TTS 播报当前疲劳等级 + 驾驶时长 */
                            int min = st.drive_seconds / 60;
                            int hh = min / 60, mm = min % 60;
                            ESP_LOGI(TAG, "Status: fatigue=%d drive=%dh%dm", g_mainboard_state.fatigue_level, hh, mm);
                            if (g_mainboard_state.fatigue_level == 2) syn6288_speak(SYN_VOICE_SEVERE_FATIGUE);
                            else if (g_mainboard_state.fatigue_level == 1) syn6288_speak(SYN_VOICE_MILD_FATIGUE);
                            else syn6288_speak(SYN_VOICE_SYS_BOOT);
                            /* 驾驶时长 ≥3h/3.5h/4h 补充提醒 */
                            if (min >= 240)      syn6288_speak(SYN_VOICE_DRIVE_4H);
                            else if (min >= 210) syn6288_speak(SYN_VOICE_DRIVE_3_5H);
                            else if (min >= 180) syn6288_speak(SYN_VOICE_DRIVE_3H);
                        } else if (strstr(cmd, "tiao gao")) {
                            syn6288_set_volume(syn6288_get_volume() + 2);
                            syn6288_speak(SYN_VOICE_VOLUME_UP);   /* 音量已调高 */
                        } else if (strstr(cmd, "tiao di")) {
                            syn6288_set_volume(syn6288_get_volume() - 2);
                            syn6288_speak(SYN_VOICE_VOLUME_DOWN);  /* 音量已调低 */
                        }
                        /* 导航到服务区: 待串网页地图 POI 显示 */
                    }
                    wake_active = false;
                    pos = 0;
                } else if (ms == ESP_MN_STATE_TIMEOUT) {
                    ESP_LOGI(TAG, "command timeout");
                    syn6288_speak(SYN_VOICE_NOT_CLEAR);   /* 没听清，请再说一次 */
                    wake_active = false;
                    pos = 0;
                } else {
                    pos = 0;
                }
            }
            if (!wake_active && pos >= s_wn_chunk) {
                static int wn_dbg = 0;
                if (++wn_dbg % 50 == 0) ESP_LOGI(TAG, "WN detect run (chunk=%d pos=%d)", s_wn_chunk, pos);
                int st = s_wn_iface->detect(s_wn_data, feed);
                if (st == 1) {
                    ESP_LOGI(TAG, ">>> WAKE WORD <<<");
                    syn6288_speak(SYN_VOICE_I_AM_HERE);   /* 唤醒回应: 我在 */
                    wake_active = true;
                    pos = 0;
                    s_mn_iface->clean(s_mn_data);
                } else {
                    pos = 0;
                }
            }
        }
    }
}

/* ── 添加自定义命令 ──
 * ⚠️ 必须用拼音！mn7_cn 的 check_speech_command 只认拼音(和 fst/commands_cn.txt 默认命令一致)。
 * esp_mn_commands_add 的中文→拼音 g2p 路径被 EN_MULTINET7_QUANT gate 住,CN 模型下传中文会 invalid。
 * 用户照样说中文,模型内部匹配拼音即可。 */
static void add_custom_commands(void)
{
    esp_mn_commands_alloc(s_mn_iface, s_mn_data);
    esp_mn_commands_add(1,  "xiu xi");                       /* 休息 */
    esp_mn_commands_add(2,  "jin ru xiu xi mo shi");         /* 进入休息模式 */
    esp_mn_commands_add(3,  "wo lei le");                    /* 我累了 */
    esp_mn_commands_add(4,  "ji xu jia shi");                /* 继续驾驶 */
    esp_mn_commands_add(5,  "tui chu xiu xi");               /* 退出休息 */
    esp_mn_commands_add(6,  "qie huan si ji");               /* 切换司机 */
    esp_mn_commands_add(7,  "cha kan zhuang tai");           /* 查看状态 */
    esp_mn_commands_add(8,  "tiao gao yin liang");           /* 调高音量 */
    esp_mn_commands_add(9,  "tiao di yin liang");            /* 调低音量 */
    esp_mn_commands_add(10, "dao hang dao zui jin fu wu qu");/* 导航到最近服务区 */
    esp_mn_commands_update();
    esp_mn_commands_print();
}

/* ═══════════════════════════════════════════
 *  对外接口
 * ═══════════════════════════════════════════ */
bool esp_sr_init(void)
{
    ESP_LOGI(TAG, "Init ESP-SR (INMP441 I2S)...");

    if (!s_ring_mutex) s_ring_mutex = xSemaphoreCreateMutex();
    if (!s_ring_mutex) { ESP_LOGE(TAG, "sem alloc fail"); return false; }

    /* 环形缓冲在 PSRAM 动态分配 */
    if (!s_ring) {
        s_ring = (int16_t *)heap_caps_malloc(RING_LEN * sizeof(int16_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ring) { ESP_LOGE(TAG, "ring alloc fail (PSRAM)"); return false; }
    }

    /* 1. 加载模型（model 分区） */
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) { ESP_LOGE(TAG, "esp_srmodel_init failed — model partition not mounted"); return false; }
    ESP_LOGI(TAG, "Models: %d", models->num);
    for (int i = 0; i < models->num; i++) ESP_LOGI(TAG, "  [%d] %s", i, models->model_name[i]);

    /* 2. WakeNet */
    char *wn_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    if (!wn_name) { ESP_LOGE(TAG, "no wakenet model"); return false; }
    ESP_LOGI(TAG, "WakeNet: %s", wn_name);
    s_wn_iface = esp_wn_handle_from_name(wn_name);
    s_wn_data = s_wn_iface->create(wn_name, DET_MODE_95);  /* 95% 灵敏度 (原 90 可能漏唤醒) */
    if (!s_wn_data) { ESP_LOGE(TAG, "wakenet create fail"); return false; }
    s_wn_chunk = s_wn_iface->get_samp_chunksize(s_wn_data);
    ESP_LOGI(TAG, "  chunk=%d rate=%d", s_wn_chunk, s_wn_iface->get_samp_rate(s_wn_data));

    /* 3. MultiNet */
    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, NULL);
    if (!mn_name) { ESP_LOGE(TAG, "no multinet model"); return false; }
    ESP_LOGI(TAG, "MultiNet: %s", mn_name);
    s_mn_iface = esp_mn_handle_from_name(mn_name);
    ESP_LOGI(TAG, "Before mn create: heap=%d int=%d psram=%d",
             esp_get_free_heap_size(),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    s_mn_data = s_mn_iface->create(mn_name, 3000);  /* 命令识别超时 3s(原 100ms 太短,唤醒后说不出命令就 timeout) */
    if (!s_mn_data) { ESP_LOGE(TAG, "multinet create fail"); return false; }
    s_mn_chunk = s_mn_iface->get_samp_chunksize(s_mn_data);
    ESP_LOGI(TAG, "  chunk=%d rate=%d", s_mn_chunk, s_mn_iface->get_samp_rate(s_mn_data));

    /* 3.5 自定义命令 (mn7 支持动态命令) */
    add_custom_commands();

    /* 4. 启动采集 + 处理任务 */
    xTaskCreate(mic_capture_task, "mic_cap", 4096, NULL, 5, NULL);
    xTaskCreate(esp_sr_process_task, "esp_sr", 8192, NULL, 6, NULL);

    ESP_LOGI(TAG, "ESP-SR ready — say wake word then command");
    return true;
}
