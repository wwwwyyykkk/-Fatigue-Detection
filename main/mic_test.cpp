/**
 * mic_test.cpp — USB 麦克风可行性测试
 * 用 usb_host_uac 组件采集 USB 麦克风音频，计算 RMS/峰值判断是否采到有效信号。
 * 触发方式：串口命令 "mictest"
 */
#include "mic_test.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"

static const char *TAG = "MicTest";

static SemaphoreHandle_t s_conn_sem = NULL;
static SemaphoreHandle_t s_rx_sem = NULL;
static uint8_t s_mic_addr = 0xFF;
static uint8_t s_mic_iface = 0xFF;
static volatile bool s_mic_found = false;
static volatile bool s_uac_installed_flag = false;

static void mic_driver_cb(uint8_t addr, uint8_t iface_num, uac_host_driver_event_t event, void *arg)
{
    if (event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED) {
        s_mic_addr = addr;
        s_mic_iface = iface_num;
        s_mic_found = true;
        ESP_LOGI(TAG, "DRIVER: USB mic RX found (addr=%u iface=%u)", addr, iface_num);
        if (s_conn_sem) xSemaphoreGiveFromISR(s_conn_sem, NULL);
    } else if (event == UAC_HOST_DRIVER_EVENT_TX_CONNECTED) {
        ESP_LOGI(TAG, "DRIVER: USB speaker TX found (iface=%u)", iface_num);
    }
}

static void mic_device_cb(uac_host_device_handle_t handle, uac_host_device_event_t event, void *arg)
{
    if (event == UAC_HOST_DEVICE_EVENT_RX_DONE) {
        if (s_rx_sem) xSemaphoreGiveFromISR(s_rx_sem, NULL);
    }
}

/* 服务 UAC 客户端事件（探测设备、RX_DONE 等）—— 必须持续调用 */
static void uac_events_task(void *arg)
{
    while (1) {
        uac_host_handle_events(portMAX_DELAY);
    }
}

/* 开机初始化：安装 UAC 驱动 + 启动事件任务
 * 必须在摄像头打开之前调用，这样 UAC 客户端能收到摄像头连接事件，麦克风一直可用 */
void mic_host_init(void)
{
    if (s_uac_installed_flag) return;

    if (!s_conn_sem) s_conn_sem = xSemaphoreCreateBinary();
    if (!s_rx_sem)   s_rx_sem   = xSemaphoreCreateBinary();

    ESP_LOGI(TAG, "Init UAC host (boot)...");
    uac_host_driver_config_t drv_cfg = {
        .callback = mic_driver_cb,
        .callback_arg = NULL,
    };
    esp_err_t err = uac_host_install(&drv_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uac_host_install failed: %s", esp_err_to_name(err));
        return;
    }
    s_uac_installed_flag = true;
    xTaskCreate(uac_events_task, "uac_events", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "UAC host installed at boot (mic will be detected on camera connect)");
}

int mic_test_run(void)
{
    ESP_LOGI(TAG, "=== USB Mic feasibility test ===");

    /* 确保 UAC 已初始化 */
    if (!s_uac_installed_flag) mic_host_init();

    if (s_mic_found) {
        xSemaphoreGive(s_conn_sem);  /* already connected */
    }
    ESP_LOGI(TAG, "Waiting for USB mic (RX)... 30s");
    if (xSemaphoreTake(s_conn_sem, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGE(TAG, "No USB mic RX in 30s.");
        ESP_LOGE(TAG, "=> 麦克风可能不是 USB UAC 设备，或摄像头没暴露音频接口");
        return -1;
    }

    ESP_LOGI(TAG, "Opening mic device (addr=%u iface=%u)...", s_mic_addr, s_mic_iface);
    uac_host_device_config_t dev_cfg = {
        .addr = s_mic_addr,
        .iface_num = s_mic_iface,
        .buffer_size = 9600,
        .buffer_threshold = 4800,
        .callback = mic_device_cb,
        .callback_arg = s_rx_sem,
    };
    uac_host_device_handle_t mic = NULL;
    esp_err_t err = uac_host_device_open(&dev_cfg, &mic);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_device_open failed: %s", esp_err_to_name(err));
        return -1;
    }
    ESP_LOGI(TAG, "Mic opened OK.");

    /* Print device params */
    uac_host_printf_device_param(mic);

    /* Use first alternate interface params */
    uac_host_dev_alt_param_t alt;
    if (uac_host_get_device_alt_param(mic, 1, &alt) != ESP_OK) {
        ESP_LOGE(TAG, "get_alt_param failed");
        return -1;
    }
    ESP_LOGI(TAG, "Mic format: %u ch, %u bit, %u Hz",
             alt.channels, alt.bit_resolution, alt.sample_freq[0]);

    const uac_host_stream_config_t stream_cfg = {
        .channels = alt.channels,
        .bit_resolution = alt.bit_resolution,
        .sample_freq = alt.sample_freq[0],
    };
    err = uac_host_device_start(mic, &stream_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_device_start failed: %s", esp_err_to_name(err));
        return -1;
    }
    uac_host_device_set_mute(mic, 0);
    uac_host_device_set_volume(mic, 80);

    ESP_LOGI(TAG, "Recording ~2s to measure signal...");
    uint32_t bytes_per_sec = alt.channels * alt.bit_resolution / 8 * alt.sample_freq[0];
    uint8_t *buf = (uint8_t *)malloc(4800);
    if (!buf) { ESP_LOGE(TAG, "buf alloc failed"); return -1; }

    uint32_t total = 0;
    double sum_sq = 0;
    int16_t peak = 0;
    uint32_t target = bytes_per_sec * 2;  /* 2 seconds */

    while (total < target) {
        if (xSemaphoreTake(s_rx_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
            ESP_LOGE(TAG, "RX timeout (no audio data arriving)");
            break;
        }
        uint32_t got = 0;
        if (uac_host_device_read(mic, buf, 4800, &got, 0) != ESP_OK) continue;
        if (got == 0) continue;

        /* 16-bit samples */
        for (uint32_t i = 0; i + 1 < got; i += 2) {
            int16_t s = (int16_t)((buf[i+1] << 8) | buf[i]);
            sum_sq += (double)s * s;
            if (s < 0) { if (-s > peak) peak = -s; }
            else       { if (s > peak) peak = s; }
            total++;
        }
    }

    double rms = (total > 0) ? sqrt(sum_sq / total) : 0;
    ESP_LOGI(TAG, "=== Result ===");
    ESP_LOGI(TAG, "Samples: %u, RMS: %.1f, Peak: %d", total, rms, peak);
    if (rms > 50) {
        ESP_LOGI(TAG, "=> 麦克风采到有效信号！RMS=%.1f (>50)", rms);
    } else if (rms > 5) {
        ESP_LOGI(TAG, "=> 有微弱信号 (RMS=%.1f)，可能环境安静或增益低", rms);
    } else {
        ESP_LOGI(TAG, "=> 几乎无信号 (RMS=%.1f)，麦克风可能没工作", rms);
    }
    ESP_LOGI(TAG, "=> 请对着麦克风说话，如果 RMS/Peak 明显变化则麦克风可用");

    free(buf);
    uac_host_device_close(mic);
    return 0;
}
