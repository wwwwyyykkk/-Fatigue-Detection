/**
 * uart_protocol.h - 双向UART通信协议
 *
 * 方向1: VirtualGPS → 主控板  (G-prefix, 每秒心跳)
 *   格式: G,speed,location,driver,duration,valid\n
 *   例:   G,80,1,1,200,1\n
 *
 * 方向2: 主控板 → VirtualGPS  (F-prefix, 状态变化+心跳)
 *   格式: F,fatigue,face_verified,alcohol,driver_confirmed,sys_status\n
 *   例:   F,2,1,0,1,1\n
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>
#include "esp_now_protocol.h"

/* ── 主控板发来的状态（F-prefix 解析结果） ── */
typedef struct {
    int fatigue_level;       /* 0=NORMAL, 1=MILD, 2=SEVERE */
    int face_verified;       /* 0=未验证, 1=已验证 */
    int alcohol_detected;    /* 0=正常, 1=超标 */
    int driver_confirmed;    /* 确认的司机ID */
    int system_status;       /* 0=初始化, 1=正常运行, 2=异常 */
    bool valid;              /* 是否收到过有效数据 */
} mainboard_state_t;

extern mainboard_state_t g_mainboard_state;
extern bool g_in_rest;    /* tracked from F-prefix identify_event, used by BUTTON_3 toggle */

/* ========== 初始化 ========== */
void uart_protocol_init(void);          /* ESP-NOW 模式：UART2 硬件已禁用 */

/** @brief WiFi 就绪后初始化 ESP-NOW（注册回调 + 添加 peer） */
esp_err_t esp_now_protocol_init_wifi(void);

/* ========== 发送 (G-prefix) ========== */
void uart_start_heartbeat_task(void);   /* 每秒心跳 */
void uart_send_now(void);               /* 立刻发送 */
void uart_send_rest_request(void);      /* 发休息请求 (ESP_NOW_CMD_REST, req=1) 给主板 */
void uart_send_rest_stop(void);         /* 发休息停止 (ESP_NOW_CMD_REST, req=0) 给主板 */
void uart_send_mock_state(void);        /* 发 Rainmaker mock 数据 (ESP_NOW_CMD_MOCK_STATE) */
void uart_send_demo_cmd(int on);        /* 发 demo ramp 命令 (ESP_NOW_CMD_DEMO, 1=开 0=关) */

/* ========== 接收 (F-prefix, 非阻塞轮询) ========== */
void uart_poll_receive(void);           /* 在主循环中调用 */

/* ========== 疲劳等级变化回调 ========== */
typedef void (*fatigue_change_cb_t)(int old_level, int new_level);
void uart_set_fatigue_change_callback(fatigue_change_cb_t cb);
