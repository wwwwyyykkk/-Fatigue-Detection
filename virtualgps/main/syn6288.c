/**
 * syn6288.c - SYN6288 TTS 驱动 (软件UART, bit-bang TX)
 *
 * 使用软件模拟 UART，不占用任何硬件 UART 外设
 * UART1 (GPIO15/16) 完全留给 GPS 使用
 *
 * 官方协议 (参照宇音天下 STC89C52/STM32):
 *   [0]    0xFD          帧头
 *   [1..2] 数据长度      = 文本长度 + 3
 *   [3]    0x01          合成并播放
 *   [4]    0x01          GBK 编码
 *   [5..N] 文本(含控制标签)
 *   [N+1]  XOR校验和     byte[0..N] 全异或
 */
#include "syn6288.h"
#include "syn6288_voices.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "SYN6288";

#define PIN_TX          GPIO_NUM_13  // 软件UART TX (任意普通GPIO即可)
#define PIN_BUSY        GPIO_NUM_7
#define BIT_US          104          // 9600bps = 104.17us/bit

static SemaphoreHandle_t s_tx_mutex = NULL;

/* ── 软件UART: 发送一个字节 ────────────────────────────── */
static void soft_uart_putc(uint8_t byte)
{
    portDISABLE_INTERRUPTS();

    /* Start bit (LOW) */
    gpio_set_level(PIN_TX, 0);
    esp_rom_delay_us(BIT_US);

    /* 8 data bits, LSB first */
    for (int i = 0; i < 8; i++) {
        gpio_set_level(PIN_TX, (byte >> i) & 1);
        esp_rom_delay_us(BIT_US);
    }

    /* Stop bit (HIGH) */
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

/* ── 当前音量 (0-15, 默认 12 = 最大声的 80%) ───────────── */
static uint8_t s_volume = 12;

void syn6288_set_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 15) vol = 15;
    s_volume = (uint8_t)vol;
}

int syn6288_get_volume(void)
{
    return (int)s_volume;
}

/* ── 构建 SYN6288 帧并发送 ────────────────────────────── */
static void syn6288_send_frame_internal(const uint8_t *gb2312_text, int text_len,
                                         bool with_ctrl_tags)
{
    /* [m4][v%d][t5]: 语速/音量控制标签(音量动态) */
    uint8_t ctrl[16];
    int ctrl_len = 0;
    if (with_ctrl_tags) {
        ctrl[ctrl_len++] = 0x5B; ctrl[ctrl_len++] = 0x6D; ctrl[ctrl_len++] = 0x34; ctrl[ctrl_len++] = 0x5D;   /* [m4] */
        ctrl[ctrl_len++] = 0x5B; ctrl[ctrl_len++] = 0x76;                                                    /* [v */
        if (s_volume >= 10) ctrl[ctrl_len++] = (uint8_t)('0' + s_volume / 10);
        ctrl[ctrl_len++] = (uint8_t)('0' + s_volume % 10);
        ctrl[ctrl_len++] = 0x5D;                                                                             /* ] */
        ctrl[ctrl_len++] = 0x5B; ctrl[ctrl_len++] = 0x74; ctrl[ctrl_len++] = 0x35; ctrl[ctrl_len++] = 0x5D;   /* [t5] */
    }
    int full_text_len = ctrl_len + text_len;
    int data_len = full_text_len + 3;   /* cmd + param + text + checksum */

    uint8_t len_hi = (data_len >> 8) & 0xFF;
    uint8_t len_lo = data_len & 0xFF;

    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "TX mutex timeout");
        return;
    }

    /* Build frame */
    uint8_t frame[256];
    int idx = 0;
    frame[idx++] = 0xFD;
    frame[idx++] = len_hi;
    frame[idx++] = len_lo;
    frame[idx++] = 0x01;              /* Command */
    frame[idx++] = 0x01;              /* GBK encoding */
    if (ctrl_len > 0) {
        memcpy(&frame[idx], ctrl, ctrl_len);
        idx += ctrl_len;
    }
    memcpy(&frame[idx], gb2312_text, text_len);
    idx += text_len;

    /* XOR checksum */
    uint8_t ecc = 0;
    for (int i = 0; i < idx; i++) ecc ^= frame[i];
    frame[idx++] = ecc;

    ESP_LOGI(TAG, "Frame: FD %02X %02X 01 01, text=%d+%d → %d bytes chk=%02X",
             len_hi, len_lo, ctrl_len, text_len, idx, ecc);

    /* Send via software UART */
    soft_uart_send(frame, idx);

    xSemaphoreGive(s_tx_mutex);

    ESP_LOGI(TAG, "Soft-UART sent %d bytes @ %d bps", idx, 1000000 / BIT_US);
    ESP_LOG_BUFFER_HEX(TAG, frame, idx > 40 ? 40 : idx);
}

static void syn6288_send_frame(const uint8_t *gb2312_text, int text_len)
{
    syn6288_send_frame_internal(gb2312_text, text_len, true);
}

/* ═══════════════════════════════════════════════════════
 *  PUBLIC API
 * ═══════════════════════════════════════════════════════ */

bool syn6288_init(void)
{
    /* TX pin: output, idle HIGH */
    gpio_config_t tx_cfg = {
        .pin_bit_mask = BIT64(PIN_TX),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&tx_cfg);
    gpio_set_level(PIN_TX, 1);

    vTaskDelay(pdMS_TO_TICKS(10));
    int level = gpio_get_level(PIN_TX);
    ESP_LOGI(TAG, "GPIO%d init: level=%d (expect HIGH=1)", PIN_TX, level);
    if (level == 0) {
        ESP_LOGW(TAG, "WARNING: GPIO%d stuck LOW — short or conflict?", PIN_TX);
    }

    /* BUSY: input with pull-down */
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

    ESP_LOGI(TAG, "Ready: TX=GPIO%d BUSY=GPIO%d 9600bps soft-UART (no HW UART used)",
             PIN_TX, PIN_BUSY);
    return true;
}

bool syn6288_speak(syn_voice_id_t id)
{
    if (id >= SYN_VOICE_COUNT) return false;
    const syn_voice_t *v = &g_syn_voices[id];
    ESP_LOGI(TAG, "Speak: %s", v->name);
    syn6288_wait_done();
    syn6288_send_frame(v->gb2312_data, v->gb2312_len);
    return true;
}

/* 动态合成: "预计%d分钟后进入轻度/严重疲劳" — 静态 GB2312 段 + ASCII 数字 */
bool syn6288_speak_prediction(int minutes, int level)
{
    if (minutes < 0) minutes = 0;
    static const uint8_t pre[]  = {0xD4,0xA4,0xBC,0xC6};                       /* 预计 */
    static const uint8_t mid[]  = {0xB7,0xD6,0xD6,0xD3,0xBA,0xF3,0xBD,0xF8,0xC8,0xEB}; /* 分钟后进入 */
    static const uint8_t mild[] = {0xC7,0xE1,0xB6,0xC8,0xC6,0xA3,0xC0,0xCD};   /* 轻度疲劳 */
    static const uint8_t sev[]  = {0xD1,0xCF,0xD6,0xD8,0xC6,0xA3,0xC0,0xCD};   /* 严重疲劳 */

    uint8_t text[64];
    int len = 0;
    memcpy(text + len, pre, sizeof(pre)); len += sizeof(pre);
    if (minutes >= 100) text[len++] = (uint8_t)('0' + (minutes / 100) % 10);
    if (minutes >= 10)  text[len++] = (uint8_t)('0' + (minutes / 10) % 10);
    text[len++] = (uint8_t)('0' + minutes % 10);
    memcpy(text + len, mid, sizeof(mid)); len += sizeof(mid);
    const uint8_t *lv = (level >= 2) ? sev : mild;
    memcpy(text + len, lv, 8); len += 8;

    ESP_LOGI(TAG, "Speak prediction: %d min -> level %d", minutes, level);
    syn6288_wait_done();
    syn6288_send_frame(text, len);
    return true;
}

bool syn6288_is_busy(void)
{
    return gpio_get_level(PIN_BUSY) == 1;
}

void syn6288_wait_done(void)
{
    int timeout = 100;
    while (syn6288_is_busy() && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ═══════════════════════════════════════════════════════
 *  波特率扫描 (使用软件UART)
 * ═══════════════════════════════════════════════════════ */

static bool syn6288_test_baud_rate(int baud_rate)
{
    if (baud_rate != 9600) {
        ESP_LOGW(TAG, "Soft-UART only supports 9600 bps, skipping %d", baud_rate);
        return false;
    }
    ESP_LOGI(TAG, "--- Testing %d bps (soft-UART) ---", baud_rate);

    /* "Hi" = FD 00 05 01 01 48 69 D9 */
    const uint8_t test_frame[] = { 0xFD, 0x00, 0x05, 0x01, 0x01, 0x48, 0x69, 0xD9 };
    ESP_LOGI(TAG, "Sending: FD 00 05 01 01 48 69 D9");
    soft_uart_send(test_frame, sizeof(test_frame));

    for (int i = 0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (syn6288_is_busy()) {
            ESP_LOGI(TAG, "  ✓ BUSY=HIGH at %dms!", (i+1)*100);
            return true;
        }
    }
    ESP_LOGI(TAG, "  No response at %d bps", baud_rate);
    return false;
}

void syn6288_scan_baud_rate(void)
{
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  SYN6288 Test (soft-UART 9600)   ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");

    if (syn6288_test_baud_rate(9600)) {
        ESP_LOGI(TAG, "*** SYN6288 RESPONDS at 9600 bps! ***");
        syn6288_speak(SYN_VOICE_SYS_BOOT);
    } else {
        ESP_LOGE(TAG, "*** NO RESPONSE ***");
        ESP_LOGE(TAG, "  [ ] SYN6288 RX ↔ GPIO%d", PIN_TX);
        ESP_LOGE(TAG, "  [ ] VCC=5V  [ ] GND↔GND  [ ] LED=ON");
        ESP_LOGE(TAG, "  USB-TTL test: FD 00 05 01 01 48 69 D9 @ 9600");
    }
}

void syn6288_run_diagnostics(void)
{
    ESP_LOGI(TAG, "\n=== SYN6288 Diag (soft-UART) ===");
    ESP_LOGI(TAG, "  TX=GPIO%d, BUSY=GPIO%d, 9600bps", PIN_TX, PIN_BUSY);
    ESP_LOGI(TAG, "  GPIO%d level=%d", PIN_TX, gpio_get_level(PIN_TX));

    /* Stop + test */
    {
        const uint8_t stop[] = { 0xFD, 0x00, 0x02, 0x02, 0xFD };
        soft_uart_send(stop, sizeof(stop));
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    {
        const uint8_t test[] = { 0xFD, 0x00, 0x05, 0x01, 0x01, 0x48, 0x69, 0xD9 };
        soft_uart_send(test, sizeof(test));
        for (int i = 0; i < 30; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (syn6288_is_busy()) { ESP_LOGI(TAG, "✓ BUSY at %dms", (i+1)*100); break; }
            if (i == 29) ESP_LOGE(TAG, "✗ No response");
        }
    }
    syn6288_speak(SYN_VOICE_SYS_BOOT);
    ESP_LOGI(TAG, "Done.\n");
}
