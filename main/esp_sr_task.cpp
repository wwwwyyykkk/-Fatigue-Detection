/**
 * esp_sr_task.cpp — ESP-SR 语音唤醒 + 命令识别
 * USB 麦克风(8kHz) → 升采样 16kHz → WakeNet 唤醒 → MultiNet 命令
 */
#include "esp_sr_task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"

/* esp-sr */
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "model_path.h"

static const char *TAG = "ESPSR";

/* ── 模型句柄 ── */
static const esp_wn_iface_t *s_wn_iface = NULL;
static model_iface_data_t   *s_wn_data  = NULL;
static int s_wn_chunk = 0;

static const esp_mn_iface_t *s_mn_iface = NULL;
static model_iface_data_t   *s_mn_data  = NULL;
static int s_mn_chunk = 0;

/* ── UAC 麦克风 ── */
static SemaphoreHandle_t s_mic_rx_sem = NULL;
static volatile bool s_mic_ready = false;
static uint8_t s_mic_addr = 0xFF, s_mic_iface = 0xFF;
static uac_host_device_handle_t s_mic_hdl = NULL;

/* ── 音频环形缓冲 (8kHz int16, FIFO) ── */
#define RING_LEN (32000)
/* 放 PSRAM：内部 RAM 紧张（WiFi/USB DMA 需要），32KB 静态数组放内部会挤爆 */
static int16_t *s_ring = NULL;
static int s_ring_w = 0;
static int s_ring_count = 0;
static SemaphoreHandle_t s_ring_mutex = NULL;

static void mic_driver_cb(uint8_t addr, uint8_t iface_num, uac_host_driver_event_t event, void *arg)
{
    if (event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED) {
        s_mic_addr = addr; s_mic_iface = iface_num; s_mic_ready = true;
        ESP_LOGI(TAG, "USB mic found (addr=%u iface=%u)", addr, iface_num);
    }
}
static void mic_dev_cb(uac_host_device_handle_t h, uac_host_device_event_t ev, void *arg)
{
    if (ev == UAC_HOST_DEVICE_EVENT_RX_DONE && s_mic_rx_sem) {
        xSemaphoreGiveFromISR(s_mic_rx_sem, NULL);
    }
}

/* ── 麦克风采集任务：打开 UAC 并连续读入环形缓冲 ── */
static void mic_capture_task(void *arg)
{
    /* buf 4800B 放 PSRAM（任务栈仅 4096，放栈上必溢出破坏堆） */
    uint8_t *buf = (uint8_t *)heap_caps_malloc(4800, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGE(TAG, "mic buf alloc fail"); vTaskDelete(NULL); return; }
    while (1) {
        if (!s_mic_hdl) {
            /* 等待麦克风检测到，然后打开并启动流 */
            vTaskDelay(pdMS_TO_TICKS(100));
            if (!s_mic_ready) continue;
            if (s_mic_hdl) continue;

            uac_host_device_config_t cfg = {
                .addr = s_mic_addr, .iface_num = s_mic_iface,
                .buffer_size = 9600, .buffer_threshold = 4800,
                .callback = mic_dev_cb, .callback_arg = s_mic_rx_sem,
            };
            if (uac_host_device_open(&cfg, &s_mic_hdl) != ESP_OK) {
                ESP_LOGE(TAG, "open mic failed"); s_mic_ready = false; continue;
            }
            uac_host_dev_alt_param_t alt;
            if (uac_host_get_device_alt_param(s_mic_hdl, 1, &alt) != ESP_OK) { continue; }
            uac_host_stream_config_t sc = {
                .channels = alt.channels, .bit_resolution = alt.bit_resolution,
                .sample_freq = alt.sample_freq[0],
            };
            if (uac_host_device_start(s_mic_hdl, &sc) != ESP_OK) { continue; }
            uac_host_device_set_mute(s_mic_hdl, 0);
            uac_host_device_set_volume(s_mic_hdl, 80);
            ESP_LOGI(TAG, "Mic streaming: %u ch %u bit %u Hz",
                     alt.channels, alt.bit_resolution, alt.sample_freq[0]);
            continue;
        }

        if (xSemaphoreTake(s_mic_rx_sem, pdMS_TO_TICKS(3000)) != pdTRUE) continue;
        uint32_t got = 0;
        if (uac_host_device_read(s_mic_hdl, buf, 4800, &got, 0) != ESP_OK) continue;
        if (got == 0) continue;
        xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
        for (uint32_t i = 0; i + 1 < got; i += 2) {
            s_ring[s_ring_w] = (int16_t)((buf[i+1] << 8) | buf[i]);
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

/* ── 8kHz → 16kHz 线性升采样 ── */
static void resample_8k_to_16k(const int16_t *in, int in_len, int16_t *out)
{
    for (int i = 0; i < in_len; i++) {
        int16_t cur = in[i];
        int16_t nxt = (i + 1 < in_len) ? in[i + 1] : cur;
        out[2*i]   = cur;
        out[2*i+1] = (int16_t)(((int32_t)cur + nxt) / 2);
    }
}

/* ── 升采样 + 唤醒词/命令处理任务 ── */
static void esp_sr_process_task(void *arg)
{
    /* raw8k+up16k+feed = 11200B，远超任务栈 8192，放 PSRAM 防栈溢出破坏堆 */
    int16_t *raw8k = (int16_t *)heap_caps_malloc(800 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *up16k = (int16_t *)heap_caps_malloc(1600 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *feed  = (int16_t *)heap_caps_malloc(3200 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw8k || !up16k || !feed) {
        ESP_LOGE(TAG, "sr buffers alloc fail");
        vTaskDelete(NULL);
        return;
    }
    const int need8 = 800;                 /* → 1600 @16k (100ms) */
    int pos = 0;
    bool wake_active = false;

    while (1) {
        if (read_ring(raw8k, need8) < need8) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        resample_8k_to_16k(raw8k, need8, up16k);

        for (int i = 0; i < need8 * 2; i++) {
            feed[pos++] = up16k[i];
            if (wake_active && pos >= s_mn_chunk) {
                esp_mn_state_t ms = s_mn_iface->detect(s_mn_data, feed);
                if (ms == ESP_MN_STATE_DETECTED) {
                    esp_mn_results_t *res = s_mn_iface->get_results(s_mn_data);
                    ESP_LOGI(TAG, ">>> COMMAND: [%s] <<<", res->string);
                    wake_active = false;
                    pos = 0;
                } else if (ms == ESP_MN_STATE_TIMEOUT) {
                    ESP_LOGI(TAG, "command timeout");
                    wake_active = false;
                    pos = 0;
                } else {
                    pos = 0;  /* shift for next chunk */
                }
            }
            if (!wake_active && pos >= s_wn_chunk) {
                int st = s_wn_iface->detect(s_wn_data, feed);
                if (st == 1) {
                    ESP_LOGI(TAG, ">>> WAKE WORD <<<");
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

/* ═══════════════════════════════════════════
 *  对外接口
 * ═══════════════════════════════════════════ */
bool esp_sr_init(void)
{
    ESP_LOGI(TAG, "Init ESP-SR...");

    if (!s_mic_rx_sem) s_mic_rx_sem = xSemaphoreCreateBinary();
    if (!s_ring_mutex) s_ring_mutex = xSemaphoreCreateMutex();
    if (!s_mic_rx_sem || !s_ring_mutex) { ESP_LOGE(TAG, "sem alloc fail"); return false; }

    /* 环形缓冲在 PSRAM 动态分配（释放内部 RAM 给 WiFi/USB DMA） */
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
    s_wn_data = s_wn_iface->create(wn_name, DET_MODE_90);
    if (!s_wn_data) { ESP_LOGE(TAG, "wakenet create fail"); return false; }
    s_wn_chunk = s_wn_iface->get_samp_chunksize(s_wn_data);
    ESP_LOGI(TAG, "  chunk=%d rate=%d", s_wn_chunk, s_wn_iface->get_samp_rate(s_wn_data));

    /* 3. MultiNet */
    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, NULL);
    if (!mn_name) { ESP_LOGE(TAG, "no multinet model"); return false; }
    ESP_LOGI(TAG, "MultiNet: %s", mn_name);
    s_mn_iface = esp_mn_handle_from_name(mn_name);
    s_mn_data = s_mn_iface->create(mn_name, 100);
    if (!s_mn_data) { ESP_LOGE(TAG, "multinet create fail"); return false; }
    s_mn_chunk = s_mn_iface->get_samp_chunksize(s_mn_data);
    ESP_LOGI(TAG, "  chunk=%d rate=%d", s_mn_chunk, s_mn_iface->get_samp_rate(s_mn_data));

    /* 4. 启动采集 + 处理任务 */
    xTaskCreate(mic_capture_task, "mic_cap", 4096, NULL, 5, NULL);
    xTaskCreate(esp_sr_process_task, "esp_sr", 8192, NULL, 6, NULL);

    ESP_LOGI(TAG, "ESP-SR ready — say wake word then command");
    return true;
}
