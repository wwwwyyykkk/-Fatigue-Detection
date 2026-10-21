/**
 * config.h - 全局配置宏定义
 *
 * 所有硬件引脚、阈值、协议参数集中在此，方便修改
 */

#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

/* ========== I2C / OLED 配置 ========== */
// SDA=GPIO4, SCL=GPIO5
#define I2C_MASTER_NUM          I2C_NUM_0
#define I2C_MASTER_SDA_IO       GPIO_NUM_4
#define I2C_MASTER_SCL_IO       GPIO_NUM_5
#define I2C_MASTER_FREQ_HZ      100000      // 100kHz
#define OLED_I2C_ADDRESS        0x3C        // SSD1306 默认地址

/* ========== ADC 按键配置 (GPIO1=ADC1_CH0, 电阻分压) ========== */
// 电路: 外部上拉~8k→VCC(~3.3V), 按键+电阻→GND
// R1=1k  R2=2k  R3=3.9k(休息请求)  R4=8.2k  R5=16k  R6=33k
// 实测(清洁电源): BTN1=0.31V BTN2=0.58V BTN3≈1.2V BTN4=1.58V BTN5=2.08V BTN6=2.52V
#define ADC_BUTTON_UNIT         ADC_UNIT_1
#define ADC_BUTTON_CHANNEL      ADC_CHANNEL_0   // GPIO1
#define ADC_BUTTON_ATTEN        ADC_ATTEN_DB_12 // 0~3.1V

/* ADC 阈值 (基于清洁电源实测) */
#define ADC_THR_BTN1_MAX    600     // BTN1: 0.31V→ADC~410
#define ADC_THR_BTN2_MAX    1100    // BTN2: 0.58V→ADC~766
#define ADC_THR_BTN3_MAX    1700    // BTN3: 3.9k → ~1.2V→ADC~1600 (休息请求)
#define ADC_THR_BTN4_MAX    2400    // BTN4: 1.58V→ADC~2087
#define ADC_THR_BTN5_MAX    3100    // BTN5: 2.08V→ADC~2748 (偶跳2.9V,平均可压住)
#define ADC_THR_BTN6_MAX    3900    // BTN6: 2.52V→ADC~3329
// >3900 = 无按键
// >4090 = 无按键(4.5V→4095)

/* 按键去抖参数 */
#define BUTTON_DEBOUNCE_MS      30          // 去抖延迟 (ms)
#define BUTTON_SCAN_INTERVAL_MS 30          // 按键扫描周期 (ms)

/* ========== UART 通信配置 ========== */
#define UART_PORT_NUM           UART_NUM_2
#define UART_TX_PIN             GPIO_NUM_17
#define UART_RX_PIN             GPIO_NUM_18         // 接收主板 F-prefix
#define UART_BAUD_RATE          115200
#define UART_BUF_SIZE           256

/* ========== WiFi 配置 (ESP-NOW 板间通信) ========== */
#define WIFI_SSID               "YOUR_WIFI_SSID"
#define WIFI_PASSWORD           "YOUR_WIFI_PASSWORD"
#define WIFI_CONNECT_TIMEOUT_MS 15000

/* ========== ESP-NOW Peer MAC (主控板 STA MAC) ========== */
#define ESP_NOW_LINK_PEER_MAC {0xe8, 0xf6, 0x0a, 0xa8, 0x9f, 0x1c}

/* ========== GPS UART 配置 (GY-GPS6MV2 / NEO-6M) ========== */
#define GPS_UART_PORT           UART_NUM_1
#define GPS_UART_TX_PIN         GPIO_NUM_15         // GPS模块RX（可选，不接也能收NMEA）
#define GPS_UART_RX_PIN         GPIO_NUM_16         // GPS模块TX ← 必须接
#define GPS_UART_BAUD_RATE      9600
#define GPS_UART_BUF_SIZE       1024

/* UART 发送周期 */
#define UART_HEARTBEAT_INTERVAL_MS  1000    // 每1秒发送一次心跳

/* ========== I2S 语音播报配置 (MAX98357) ========== */
#define I2S_BCK_PIN             GPIO_NUM_18
#define I2S_LRC_PIN             GPIO_NUM_19
#define I2S_DIN_PIN             GPIO_NUM_20

/* ========== 默认初始状态 ========== */
#define DEFAULT_SPEED_KMH       0.0f
#define DEFAULT_LOCATION        LOCATION_NORMAL
#define DEFAULT_DRIVER_ID       1

/* ========== GPS 坐标默认值（南京集成电路培训中心附近, 七里桥北路6号） ========== */
#define DEFAULT_GPS_LAT         32.105000f
#define DEFAULT_GPS_LNG         118.705000f

/* ========== 自动走时配置 ========== */
#define TIME_TICK_INTERVAL_MS   1000    // 每秒递增驾驶时间

/* ========== MQ3 酒精传感器配置 ========== */
#define MQ3_ADC_UNIT           ADC_UNIT_1
#define MQ3_ADC_CHANNEL        ADC_CHANNEL_1   // GPIO2 = ADC1_CH1
#define MQ3_ADC_ATTEN          ADC_ATTEN_DB_12 // 0~3.1V
#define MQ3_DO_PIN             GPIO_NUM_6      // 数字输出 (LOW=检测到酒精)
#define MQ3_ADC_THRESHOLD      2500            // ADC 阈值 (~1.9V)，超过即触发
#define MQ3_COOLDOWN_MS        30000           // 告警冷却时间 (30秒)
#define MQ3_SCAN_INTERVAL_MS   500             // 扫描间隔
