/**
 * syn6288.c - SYN6288 软件UART驱动实现
 *
 * 软件UART: 9600bps 8N1, 纯发送
 * 位时序: 1/9600 = 104.17us, 用 esp_rom_delay_us(104) 近似
 */
#include "syn6288.h"
#include "syn6288_voices.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "SYN6288";

#define PIN_TX      GPIO_NUM_45  // 换到 GPIO45 测试（STRAPPING 引脚，但启动后可用）
#define PIN_BUSY    GPIO_NUM_7
#define BIT_US      104     /* 9600bps = 104.17us per bit */

static SemaphoreHandle_t s_tx_mutex = NULL;

/* ── 软件UART 发送一个字节 ────────────────────────────── */
static void soft_uart_putc(uint8_t byte)
{
    /* Disable interrupts during bit-bang for clean timing */
    portDISABLE_INTERRUPTS();

    /* Start bit (low) */
    gpio_set_level(PIN_TX, 0);
    esp_rom_delay_us(BIT_US);

    /* 8 data bits, LSB first */
    for (int i = 0; i < 8; i++) {
        gpio_set_level(PIN_TX, (byte >> i) & 1);
        esp_rom_delay_us(BIT_US);
    }

    /* Stop bit (high) */
    gpio_set_level(PIN_TX, 1);
    esp_rom_delay_us(BIT_US);

    portENABLE_INTERRUPTS();
}

/* ── 发送字节数组 ─────────────────────────────────────── */
static void soft_uart_send(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        soft_uart_putc(data[i]);
    }
}

/* ── 构建 SYN6288 帧并发送 ────────────────────────────── */
static void syn6288_send_frame(const uint8_t *gb2312_text, int text_len)
{
    /*
     * SYN6288 帧格式:
     *   0xFD          帧头
     *   len_hi        数据长度高字节
     *   len_lo        数据长度低字节
     *   0x01          命令: 语音合成播放
     *   0x00          保留
     *   [控制标签]    如 [m4][v16][t5]
     *   [GB2312文本]
     *
     * 数据长度 = 命令(1) + 保留(1) + 控制标签 + GB2312文本
     */

    /* 控制标签: [m4]背景音量中 [v16]朗读最大 [t5]最快语速 */
    const uint8_t ctrl[] = {
        0x5B, 0x6D, 0x34, 0x5D,     /* [m4] */
        0x5B, 0x76, 0x31, 0x36, 0x5D, /* [v16] */
        0x5B, 0x74, 0x35, 0x5D,     /* [t5] */
    };

    int data_len = 1 + 1 + sizeof(ctrl) + text_len;  /* cmd + reserve + ctrl + text */
    uint8_t len_hi = (data_len >> 8) & 0xFF;
    uint8_t len_lo = data_len & 0xFF;

    /* Take mutex — one frame at a time */
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "TX mutex timeout");
        return;
    }

    ESP_LOGI(TAG, "Sending frame: FD %02X %02X 01 00 + ctrl(%d) + text(%d)",
             len_hi, len_lo, sizeof(ctrl), text_len);

    /* Send frame */
    soft_uart_putc(0xFD);       /* Header */
    soft_uart_putc(len_hi);     /* Length high */
    soft_uart_putc(len_lo);     /* Length low */
    soft_uart_putc(0x01);       /* Command: play */
    soft_uart_putc(0x00);       /* Reserved */
    soft_uart_send(ctrl, sizeof(ctrl));
    soft_uart_send(gb2312_text, text_len);

    xSemaphoreGive(s_tx_mutex);

    ESP_LOGI(TAG, "Frame sent: %d bytes total", 5 + data_len);

    /* 打印发送的文本（hex dump）*/
    ESP_LOG_BUFFER_HEX(TAG, gb2312_text, text_len);
}

/* ── 预设计算帧的 CRC (SYN6288 不需要，但留接口) ──────── */

/* ═══════════════════════════════════════════════════════
 *  PUBLIC API
 * ═══════════════════════════════════════════════════════ */

bool syn6288_init(void)
{
    /* TX pin: output, idle high */
    gpio_config_t tx_cfg = {
        .pin_bit_mask = BIT64(PIN_TX),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&tx_cfg);
    gpio_set_level(PIN_TX, 1);  /* idle high */

    /* ========== 调试：检查 GPIO 是否正常 ========== */
    vTaskDelay(pdMS_TO_TICKS(10));  // 延迟 10ms
    int level = gpio_get_level(PIN_TX);
    ESP_LOGI(TAG, "GPIO%d init check: set HIGH, read back = %d", PIN_TX, level);
    if (level == 0) {
        ESP_LOGW(TAG, "WARNING: GPIO%d stuck at LOW! Check for conflicts or short circuit!", PIN_TX);
    }
    /* ============================================== */

    /* BUSY pin: input (optional, use pull-down so reads low if unconnected) */
    gpio_config_t busy_cfg = {
        .pin_bit_mask = BIT64(PIN_BUSY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&busy_cfg);

    s_tx_mutex = xSemaphoreCreateMutex();
    if (!s_tx_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return false;
    }

    ESP_LOGI(TAG, "Ready: TX=GPIO%d, BUSY=GPIO%d, 9600bps soft-UART",
             PIN_TX, PIN_BUSY);
    return true;
}

bool syn6288_speak(syn_voice_id_t id)
{
    if (id >= SYN_VOICE_COUNT) return false;

    const syn_voice_t *v = &g_syn_voices[id];
    ESP_LOGI(TAG, "Speak: %s", v->name);

    /* Wait for previous play to finish, if any */
    syn6288_wait_done();

    syn6288_send_frame(v->gb2312_data, v->gb2312_len);
    return true;
}

bool syn6288_is_busy(void)
{
    return gpio_get_level(PIN_BUSY) == 1;
}

void syn6288_wait_done(void)
{
    /* Wait up to 10 seconds for BUSY to go low */
    int timeout = 100;
    while (syn6288_is_busy() && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
