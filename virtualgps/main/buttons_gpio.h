/**
 * buttons_gpio.h - GPIO按键驱动接口
 *
 * 提供统一的按键抽象层，方便后续切换到ADC按键
 */

#pragma once

#include <stdbool.h>

/* ========== 按键ID枚举 ========== */
typedef enum {
    BUTTON_NONE = 0,
    BUTTON_1,       // 速度循环 0→80→150
    BUTTON_2,       // 地点循环切换
    BUTTON_3,       // 休息请求 (本地检查停车+服务区，满足则发主板)
    BUTTON_4,       // 真实/虚拟GPS切换
    BUTTON_5,       // 司机切换
    BUTTON_6        // 驾驶时长 +30min
} button_id_t;

/* ========== 初始化按键驱动 ========== */
void buttons_init(void);

/* ========== 启动按键扫描任务 ========== */
void buttons_start_scan_task(void);

/* ========== 按键扫描 (返回当前按下的按键ID，无按键返回 BUTTON_NONE) ========== */
button_id_t buttons_scan(void);

/* ========== 休息请求 (公开接口，Rainmaker/write_cb 复用) ========== */
void buttons_handle_rest_request(void);
