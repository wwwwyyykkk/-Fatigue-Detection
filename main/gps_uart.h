/**
 * gps_uart.h - 双向通信: ESP-NOW(VirtualGPS) + UART1(EG800K)
 *
 * ESP-NOW: VirtualGPS (cmd 0x10 收 + cmd 0x20 发) via WiFi
 * UART1:   EG800K (合并数据发 + GPS回传收) @ 115200bps
 *
 * 原 UART0 VirtualGPS 有线通信已替换为 ESP-NOW，UART0 释放给 PC 调试。
 *
 * Receive (G-prefix via ESP-NOW cmd 0x10): gps_data_msg_t (18 bytes binary)
 * Transmit (F-prefix via ESP-NOW cmd 0x20): fatigue_state_msg_t (5 bytes binary)
 * Transmit (EG800K UART1):   speed,location,driver,dur,fatigue,face,alc,drv,sys\n
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>
#include "esp_now_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize UART1(EG800K) only. UART0(VirtualGPS) replaced by ESP-NOW.
 * @param eg800k_rx EG800K RX pin (connect to EG800K TX)
 * @param eg800k_tx EG800K TX pin (connect to EG800K RX)
 */
void gps_uart_init(int eg800k_rx, int eg800k_tx);

/* ── ESP-NOW protocol (replaces UART0 VirtualGPS) ── */

/** @brief WiFi 就绪后初始化 ESP-NOW（注册回调 + 添加 peer） */
esp_err_t esp_now_protocol_init(void);

/** @brief 发送 F-prefix via ESP-NOW (cmd 0x20) */
void esp_now_protocol_send_f_state(int fatigue_level, int face_verified,
                                   int alcohol_detected, int driver_confirmed,
                                   int system_status, int identify_event,
                                   int pred_minutes, int pred_level);

/** @brief Poll: 超时检测 + 同步 g_display_data.gps_* (替代 gps_uart_poll) */
void esp_now_protocol_poll(void);

/* ── EG800K UART1 (unchanged) ── */

/**
 * @brief Poll UART1 for incoming GPS data from EG800K (non-blocking)
 */
void gps_uart_poll_eg800k(void);

/**
 * @brief Check if VirtualGPS is connected (received data recently via ESP-NOW)
 */
bool gps_uart_is_connected(void);

/**
 * @brief Send combined data to EG800K (VirtualGPS state + fatigue data)
 */
void gps_uart_send_to_eg800k(int fatigue_level, int face_verified,
                             int driver_confirmed, int system_status,
                             float perclos, int alcohol,
                             int rest_dur_min, int rest_recov_pct);

/* GPS data getters (from received ESP-NOW G-prefix) */
float gps_uart_get_speed(void);
int   gps_uart_get_location(void);
int   gps_uart_get_driver_id(void);
int   gps_uart_get_drive_seconds(void);
float gps_uart_get_lat(void);
float gps_uart_get_lng(void);

/* Rainmaker mock perclos 覆盖: -1=不覆盖(用真实值), >=0=覆盖显示/上报值 */
int gps_uart_get_mock_perclos(void);

/* ── Deprecated (commented out, replaced by ESP-NOW) ── */
// void gps_uart_poll(void);          /* → esp_now_protocol_poll() */
// void gps_uart_send_state(...);     /* → esp_now_protocol_send_f_state() */

#ifdef __cplusplus
}
#endif
