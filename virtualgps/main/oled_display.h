/**
 * oled_display.h - SSD1306 OLED 显示接口
 *
 * 显示4行信息:
 *   Line 1: Speed: 80 km/h
 *   Line 2: Loc: Service Area
 *   Line 3: Driver: #1
 *   Line 4: Drive: 3h20min
 */

#pragma once

/* ========== 初始化OLED (I2C + SSD1306驱动) ========== */
void oled_display_init(void);

/* ========== 刷新OLED显示内容 (读取当前GPS状态并渲染) ========== */
void oled_display_update(void);

/* ========== 启动OLED刷新任务 (等待状态变化事件后刷新) ========== */
void oled_start_refresh_task(void);
