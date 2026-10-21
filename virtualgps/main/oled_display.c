/**
 * oled_display.c - SSD1306 OLED 显示驱动实现
 *
 * I2C 驱动 SSD1306 (128x64)
 * 中文 16x16 点阵 + ASCII 5x7 混合显示
 * 中文使用 GB2312 编码（C源码用转义字节，避免编码问题）
 */

#include "oled_display.h"
#include "gps_state.h"
#include "uart_protocol.h"
#include "config.h"
#include "chinese_font.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static const char *TAG = "OLED";

/* ========== SSD1306 命令 ========== */
#define OLED_CMD_SET_CONTRAST        0x81
#define OLED_CMD_DISPLAY_ALL_ON_RESUME 0xA4
#define OLED_CMD_DISPLAY_NORMAL      0xA6
#define OLED_CMD_DISPLAY_INVERSE     0xA7
#define OLED_CMD_DISPLAY_OFF         0xAE
#define OLED_CMD_DISPLAY_ON          0xAF
#define OLED_CMD_SET_DISPLAY_OFFSET  0xD3
#define OLED_CMD_SET_COM_PINS        0xDA
#define OLED_CMD_SET_VCOM_DETECT     0xDB
#define OLED_CMD_SET_DISPLAY_CLOCK_DIV 0xD5
#define OLED_CMD_SET_PRECHARGE       0xD9
#define OLED_CMD_SET_MULTIPLEX       0xA8
#define OLED_CMD_SET_START_LINE      0x40
#define OLED_CMD_MEMORY_MODE         0x20
#define OLED_CMD_COLUMN_ADDR         0x21
#define OLED_CMD_PAGE_ADDR           0x22
#define OLED_CMD_COM_SCAN_DEC        0xC8
#define OLED_CMD_SEG_REMAP           0xA1
#define OLED_CMD_CHARGE_PUMP         0x8D

/* ========== GB2312 中文标签（用字节数组避免编码问题） ========== */
static const uint8_t GB_SPEED[]     = {0xCB, 0xD9, 0xB6, 0xC8, 0x00}; // 速度
static const uint8_t GB_LOCATION[]  = {0xCE, 0xBB, 0xD6, 0xC3, 0x00}; // 位置
static const uint8_t GB_DRIVER[]    = {0xCB, 0xBE, 0xBB, 0xFA, 0x00}; // 司机
static const uint8_t GB_IDENTIFYING[] = {0xCA, 0xB6, 0xB1, 0xF0, 0xD6, 0xD0, 0x2E, 0x2E, 0x2E, 0x00}; // 识别中...
static const uint8_t GB_DRIVE[]     = {0xBC, 0xDD, 0xCA, 0xBB, 0xCA, 0xB1, 0xB3, 0xA4, 0x00}; // 驾驶时长

/* 地点名 */
static const uint8_t GB_LOC_NORMAL[]    = {0xC6, 0xD5, 0xCD, 0xA8, 0xC2, 0xB7, 0xB6, 0xCE, 0x00}; // 普通路段
static const uint8_t GB_LOC_SERVICE[]   = {0xB7, 0xFE, 0xCE, 0xF1, 0xC7, 0xF8, 0x00};             // 服务区
static const uint8_t GB_LOC_EMERGENCY[] = {0xD3, 0xA6, 0xBC, 0xB1, 0xB3, 0xB5, 0xB5, 0xC0, 0x00}; // 应急车道

/* ========== I2C 写命令 ========== */
static esp_err_t oled_write_cmd(uint8_t cmd)
{
    uint8_t data[2] = {0x00, cmd};
    return i2c_master_write_to_device(I2C_MASTER_NUM, OLED_I2C_ADDRESS, data, 2, pdMS_TO_TICKS(100));
}

/* ========== I2C 写数据 ========== */
static esp_err_t oled_write_data(uint8_t *data, size_t len)
{
    uint8_t *buf = malloc(len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    buf[0] = 0x40;
    memcpy(buf + 1, data, len);
    esp_err_t ret = i2c_master_write_to_device(I2C_MASTER_NUM, OLED_I2C_ADDRESS, buf, len + 1, pdMS_TO_TICKS(100));
    free(buf);
    return ret;
}

/* ========== 显存 ========== */
static uint8_t s_display_buffer[128 * 8];

/* ========== 5x7 ASCII 字库 ========== */
static const uint8_t font5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // ' '
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // '!'
    {0x00, 0x07, 0x00, 0x07, 0x00}, // '"'
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // '#'
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // '$'
    {0x23, 0x13, 0x08, 0x64, 0x62}, // '%'
    {0x36, 0x49, 0x55, 0x22, 0x50}, // '&'
    {0x00, 0x05, 0x03, 0x00, 0x00}, // '\''
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // '('
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // ')'
    {0x14, 0x08, 0x3E, 0x08, 0x14}, // '*'
    {0x08, 0x08, 0x08, 0x08, 0x08}, // '+'
    {0x00, 0x50, 0x30, 0x00, 0x00}, // ','
    {0x08, 0x08, 0x08, 0x08, 0x08}, // '-'
    {0x00, 0x60, 0x60, 0x00, 0x00}, // '.'
    {0x20, 0x10, 0x08, 0x04, 0x02}, // '/'
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // '0'
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // '1'
    {0x42, 0x61, 0x51, 0x49, 0x46}, // '2'
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // '3'
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // '4'
    {0x27, 0x45, 0x45, 0x45, 0x39}, // '5'
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // '6'
    {0x01, 0x71, 0x09, 0x05, 0x03}, // '7'
    {0x36, 0x49, 0x49, 0x49, 0x36}, // '8'
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // '9'
    {0x00, 0x36, 0x36, 0x00, 0x00}, // ':'
    {0x00, 0x56, 0x36, 0x00, 0x00}, // ';'
    {0x08, 0x14, 0x22, 0x41, 0x00}, // '<'
    {0x14, 0x14, 0x14, 0x14, 0x14}, // '='
    {0x00, 0x41, 0x22, 0x14, 0x08}, // '>'
    {0x02, 0x01, 0x51, 0x09, 0x06}, // '?'
    {0x32, 0x49, 0x79, 0x41, 0x3E}, // '@'
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // 'A'
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // 'B'
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // 'C'
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // 'D'
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // 'E'
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // 'F'
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // 'G'
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // 'H'
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // 'I'
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // 'J'
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // 'K'
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // 'L'
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // 'M'
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // 'N'
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // 'O'
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // 'P'
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // 'Q'
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // 'R'
    {0x46, 0x49, 0x49, 0x49, 0x31}, // 'S'
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // 'T'
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // 'U'
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // 'V'
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // 'W'
    {0x63, 0x14, 0x08, 0x14, 0x63}, // 'X'
    {0x07, 0x08, 0x70, 0x08, 0x07}, // 'Y'
    {0x61, 0x51, 0x49, 0x45, 0x43}, // 'Z'
};

/* ========== 绘制中文 16x16 字符 ========== */
static void draw_chinese_16(int x, int y, const uint8_t *bitmap)
{
    if (!bitmap) return;
    for (int row = 0; row < 16; row++) {
        int page = (y + row) / 8;
        if (page >= 8) break;
        int bit_in_page = (y + row) % 8;
        uint16_t line = ((uint16_t)bitmap[row * 2] << 8) | bitmap[row * 2 + 1];
        for (int col = 0; col < 16; col++) {
            int px = x + col;
            if (px >= 128) break;
            if (line & (1 << (15 - col))) {
                s_display_buffer[page * 128 + px] |= (1 << bit_in_page);
            }
        }
    }
}

/* ========== 查找并绘制 GB2312 中文 ========== */
static void draw_gb_double(int x, int y, uint8_t hi, uint8_t lo)
{
    uint16_t gb = ((uint16_t)hi << 8) | lo;
    const uint8_t *bitmap = chinese_font_lookup(gb);
    if (bitmap) {
        draw_chinese_16(x, y, bitmap);
    }
}

/* ========== 绘制 GB2312 字符串（以 0x00 结尾的字节数组） ========== */
static void draw_gb_str(int x, int y, const uint8_t *str)
{
    while (*str) {
        draw_gb_double(x, y, str[0], str[1]);
        x += 16;
        str += 2;
    }
}

/* ========== 绘制 ASCII 字符 (5x7) ========== */
static void draw_ascii_char(int x, int y, char ch)
{
    if (ch < 32 || ch > 90) ch = ' ';
    const uint8_t *glyph = font5x7[ch - 32];

    // 优化：当 y 是 8 的倍数时，直接整页写入
    if (y % 8 == 0) {
        int page = y / 8;
        if (page >= 8) return;
        for (int i = 0; i < 5; i++) {
            int col = x + i;
            if (col >= 128) break;
            s_display_buffer[page * 128 + col] = glyph[i];
        }
        return;
    }

    // 非对齐：逐行写入，处理跨页
    for (int row = 0; row < 7; row++) {
        int py = y + row;
        int page = py / 8;
        int bit = py % 8;
        if (page >= 8) break;
        for (int i = 0; i < 5; i++) {
            int col = x + i;
            if (col >= 128) break;
            if (glyph[i] & (1 << row)) {
                s_display_buffer[page * 128 + col] |= (1 << bit);
            }
        }
    }
}

/* ========== 绘制 ASCII 字符串 ========== */
static void draw_ascii_str(int x, int y, const char *str)
{
    while (*str) {
        draw_ascii_char(x, y, *str);
        x += 6;
        str++;
    }
}

/* ========== I2C 初始化 ========== */
static void i2c_master_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

/* ========== SSD1306 初始化 ========== */
void oled_display_init(void)
{
    i2c_master_init();
    ESP_LOGI(TAG, "I2C SDA=GPIO%d SCL=GPIO%d", I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_err_t ret = oled_write_cmd(OLED_CMD_DISPLAY_OFF);
    if (ret == ESP_OK) ESP_LOGI(TAG, "OLED ACK OK");
    else ESP_LOGE(TAG, "OLED NOT RESPONDING err=%d", ret);

    oled_write_cmd(OLED_CMD_DISPLAY_OFF);
    oled_write_cmd(OLED_CMD_SET_DISPLAY_CLOCK_DIV); oled_write_cmd(0x80);
    oled_write_cmd(OLED_CMD_SET_MULTIPLEX); oled_write_cmd(0x3F);
    oled_write_cmd(OLED_CMD_SET_DISPLAY_OFFSET); oled_write_cmd(0x00);
    oled_write_cmd(OLED_CMD_SET_START_LINE | 0x00);
    oled_write_cmd(OLED_CMD_CHARGE_PUMP); oled_write_cmd(0x14);
    oled_write_cmd(OLED_CMD_MEMORY_MODE); oled_write_cmd(0x00);
    oled_write_cmd(OLED_CMD_SEG_REMAP | 0x01);
    oled_write_cmd(OLED_CMD_COM_SCAN_DEC);
    oled_write_cmd(OLED_CMD_SET_COM_PINS); oled_write_cmd(0x12);
    oled_write_cmd(OLED_CMD_SET_CONTRAST); oled_write_cmd(0xCF);
    oled_write_cmd(OLED_CMD_SET_PRECHARGE); oled_write_cmd(0xF1);
    oled_write_cmd(OLED_CMD_SET_VCOM_DETECT); oled_write_cmd(0x40);
    oled_write_cmd(OLED_CMD_DISPLAY_ALL_ON_RESUME);
    oled_write_cmd(OLED_CMD_DISPLAY_INVERSE);  // 白底黑字 (改为 NORMAL 恢复)
    oled_write_cmd(OLED_CMD_DISPLAY_ON);

    memset(s_display_buffer, 0, sizeof(s_display_buffer));
    oled_write_cmd(OLED_CMD_COLUMN_ADDR); oled_write_cmd(0); oled_write_cmd(127);
    oled_write_cmd(OLED_CMD_PAGE_ADDR); oled_write_cmd(0); oled_write_cmd(7);
    oled_write_data(s_display_buffer, sizeof(s_display_buffer));
    ESP_LOGI(TAG, "OLED initialized (SSD1306 128x64)");
}

/* ========== 刷新显存 ========== */
static void refresh_display(void)
{
    oled_write_cmd(OLED_CMD_COLUMN_ADDR); oled_write_cmd(0); oled_write_cmd(127);
    oled_write_cmd(OLED_CMD_PAGE_ADDR); oled_write_cmd(0); oled_write_cmd(7);
    oled_write_data(s_display_buffer, sizeof(s_display_buffer));
}

/* ========== 获取地点中文名 ========== */
static const uint8_t *location_gb(location_type_t loc)
{
    switch (loc) {
        case LOCATION_NORMAL:        return GB_LOC_NORMAL;
        case LOCATION_SERVICE_AREA:  return GB_LOC_SERVICE;
        case LOCATION_EMERGENCY_LANE: return GB_LOC_EMERGENCY;
        default:                     return GB_LOC_NORMAL;
    }
}

/* ========== 刷新 OLED 显示 ========== */
void oled_display_update(void)
{
    virtual_gps_state_t state;
    gps_state_read(&state);

    memset(s_display_buffer, 0, sizeof(s_display_buffer));

    char str[24];

    // ── 第1行: 速度 000KM/H (y=0) ──
    draw_gb_str(0, 0, GB_SPEED);          // 速度
    snprintf(str, sizeof(str), "%03.0f", state.speed_kmh);
    draw_ascii_str(36, 4, str);           // 数字
    draw_ascii_str(56, 4, "KM/H");        // 紧挨着数字

    // ── 第2行: 位置 普通路段/服务区/应急车道 (y=16) ──
    draw_gb_str(0, 16, GB_LOCATION);
    draw_gb_str(36, 16, location_gb(state.location));

    // ── 第3行: 司机1 [R/V]N39 54 E116 23 (y=32) ──
    draw_gb_str(0, 32, GB_DRIVER);         // 司机
    if (g_mainboard_state.valid && g_mainboard_state.face_verified == 0) {
        draw_gb_str(34, 32, GB_IDENTIFYING);  // 识别中...（占满余下行）
    } else {
        snprintf(str, sizeof(str), "%d", state.driver_id);
        draw_ascii_str(34, 36, str);

        // GPS 来源标识: R=真实有信号 / !=真实无信号 / V=虚拟
        char gps_indicator;
        if (state.use_real_gps) {
            gps_indicator = state.has_gps_fix ? 'R' : '!';
        } else {
            gps_indicator = 'V';
        }
        draw_ascii_char(38, 36, gps_indicator);

        // N纬度 (dd mm 格式，数字间加空格)
        draw_ascii_str(44, 36, "N");
        int lat_d = (int)state.gps_lat;
        float lat_mf = fabsf(state.gps_lat - lat_d) * 60.0f;
        int lat_m = (int)lat_mf;
        snprintf(str, sizeof(str), "%02d", abs(lat_d));
        draw_ascii_str(52, 36, str);           // 度
        snprintf(str, sizeof(str), "%02d", lat_m);
        draw_ascii_str(66, 36, str);           // 分（加空格）

        // E经度 (ddd mm 格式)
        draw_ascii_str(82, 36, "E");
        int lng_d = (int)state.gps_lng;
        float lng_mf = fabsf(state.gps_lng - lng_d) * 60.0f;
        int lng_m = (int)lng_mf;
        snprintf(str, sizeof(str), "%03d", abs(lng_d));
        draw_ascii_str(90, 36, str);           // 度
        snprintf(str, sizeof(str), "%02d", lng_m);
        draw_ascii_str(110, 36, str);          // 分（加空格）
    }

    // ── 第4行: 驾驶时长 00:00:00 (y=46, shifted up to make room for bar) ──
    draw_gb_str(0, 46, GB_DRIVE);
    int h = state.drive_seconds / 3600;
    int m = (state.drive_seconds % 3600) / 60;
    int s = state.drive_seconds % 60;
    snprintf(str, sizeof(str), "%02d:%02d:%02d", h, m, s);
    draw_ascii_str(68, 50, str);

    /* ── Fatigue status bar (bottom 2 pixels) ── */
    if (g_mainboard_state.valid) {
        uint8_t mask;
        switch (g_mainboard_state.fatigue_level) {
            case 2: mask = 0xFC; break;   /* severe: thick bar  4px y=60-63 */
            case 1: mask = 0xC0; break;   /* mild:   medium bar 2px y=62-63 */
            default:mask = 0x80; break;   /* normal: thin line  1px y=63 */
        }
        for (int x = 0; x < 128; x++) {
            s_display_buffer[7 * 128 + x] |= mask;  /* page 7 = y=56-63 */
        }
    }

    refresh_display();
}

/* ========== OLED 刷新任务 ========== */
static void oled_refresh_task(void *arg)
{
    ESP_LOGI(TAG, "OLED refresh task started");
    oled_display_update();

    TickType_t last = xTaskGetTickCount();
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
            g_state_event, STATE_CHANGED_BIT,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));

        TickType_t now = xTaskGetTickCount();
        if ((bits & STATE_CHANGED_BIT) || (now - last >= pdMS_TO_TICKS(1000))) {
            oled_display_update();
            last = now;
        }
    }
}

void oled_start_refresh_task(void)
{
    xTaskCreate(oled_refresh_task, "oled_refresh", 4096, NULL, 2, NULL);
}
