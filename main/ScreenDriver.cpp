/**
 * ScreenDriver.cpp - ST7789 panel init (LVGL-compatible)
 * Only initializes the SPI bus and ST7789 panel.
 * Frame buffer and drawing are handled by LVGL + esp_lvgl_port.
 */
#include "ScreenDriver.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "esp_log.h"

static const char *TAG = "Screen";

namespace {
constexpr int TFT_MOSI_PIN = 11;
constexpr int TFT_SCLK_PIN = 12;
constexpr int TFT_CS_PIN   = 8;
constexpr int TFT_DC_PIN   = 10;
constexpr int TFT_RST_PIN  = 9;
constexpr int LCD_H_RES = 240;
constexpr int LCD_V_RES = 320;
constexpr int LCD_PIXEL_CLOCK_HZ = 16 * 1000 * 1000;
}

esp_lcd_panel_handle_t screen_init_panel(esp_lcd_panel_io_handle_t *out_io)
{
    ESP_LOGI(TAG, "Initializing ST7789 panel (LVGL mode)");
    ESP_LOGI(TAG, "Pins: MOSI=%d SCLK=%d CS=%d DC=%d RST=%d",
             TFT_MOSI_PIN, TFT_SCLK_PIN, TFT_CS_PIN, TFT_DC_PIN, TFT_RST_PIN);

    /* SPI bus */
    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = TFT_SCLK_PIN;
    buscfg.mosi_io_num = TFT_MOSI_PIN;
    buscfg.miso_io_num = -1;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = LCD_H_RES * 20 * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_DISABLED));

    /* Panel IO */
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num = TFT_DC_PIN;
    io_config.cs_gpio_num = TFT_CS_PIN;
    io_config.pclk_hz = LCD_PIXEL_CLOCK_HZ;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.spi_mode = 0;
    io_config.trans_queue_depth = 10;
    io_config.on_color_trans_done = nullptr;
    io_config.user_ctx = nullptr;

    esp_lcd_panel_io_handle_t io_handle = nullptr;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                              &io_config, &io_handle));

    /* ST7789 panel */
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = TFT_RST_PIN;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_config.bits_per_pixel = 16;

    esp_lcd_panel_handle_t panel = nullptr;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    /* 2026-08-04: invert ON → colors become WYSIWYG (INVOFF showed complement).
     * UI code in lvgl_ui.cpp now uses normal color values. */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    if (out_io) *out_io = io_handle;
    ESP_LOGI(TAG, "ST7789 panel ready (%dx%d)", LCD_H_RES, LCD_V_RES);
    return panel;
}
