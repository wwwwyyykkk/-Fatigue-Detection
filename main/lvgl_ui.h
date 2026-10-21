/** lvgl_ui.h - LVGL-based TFT display UI */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Init LVGL task infrastructure (call from app_main, before camera) */
void lvgl_ui_init_task(void);

/** @brief Init display panel + create UI pages (call after camera opens) */
void lvgl_ui_init_display(void);

#ifdef __cplusplus
}
#endif
