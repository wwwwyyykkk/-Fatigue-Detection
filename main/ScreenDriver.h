/**
 * ScreenDriver.h - ST7789 LCD panel init (LVGL-compatible)
 * Initializes SPI bus and ST7789 panel, returns both handles for LVGL port.
 */
#pragma once
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Initialize ST7789 panel for LVGL. Returns panel handle; io_handle via out param. */
esp_lcd_panel_handle_t screen_init_panel(esp_lcd_panel_io_handle_t *out_io);

#ifdef __cplusplus
}
#endif
