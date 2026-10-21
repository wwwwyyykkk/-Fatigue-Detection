#pragma once
// ================================================================
//  驾盾 DriveSafe Guardian — 全局配置
//  硬件: ESP32-S3-DevKitC-1 + OV5640 + SSD1306 + MQ-3 + EG800K
// ================================================================

// ─── 引脚定义 ────────────────────────────────────────────────────

// OV5640 DVP
#define CAM_PWDN    15
#define CAM_RESET    5
#define CAM_XCLK    14
#define CAM_SIOD     2
#define CAM_SIOC     4
#define CAM_Y7      16
#define CAM_Y6      17
#define CAM_Y5      18
#define CAM_Y4      12
#define CAM_Y3      10
#define CAM_Y2       8
#define CAM_Y1       9
#define CAM_Y0      11
#define CAM_VSYNC    6
#define CAM_HREF     7
#define CAM_PCLK    13

// SSD1306 I2C（与摄像头 I2C 复用同一总线，地址 0x3C）
#define OLED_SDA     2
#define OLED_SCL     4

// 蜂鸣器（无源，PWM）
#define BUZZER_PIN  38

// MQ-3 酒精传感器（ADC）
#define MQ3_PIN     39   // GPIO39 = ADC1_CH3

// EG800K 4G/GPS（UART1）
#define EG800K_TX   43
#define EG800K_RX   44
#define EG800K_BAUD 115200

// 按键
#define BTN_TRIP    35   // 行程开始/结束
#define BTN_ACK     36   // 疲劳解除确认

// ─── AI 模型阈值 ──────────────────────────────────────────────────
#define FACE_CONF_THRESHOLD   0.80f   // 人脸识别通过阈值
#define FACE_VERIFY_TIMEOUT   10000   // 验证超时 ms
#define EAR_CLOSED_THRESHOLD  0.25f   // EAR 低于此值 = 闭眼
#define MAR_YAWN_THRESHOLD    0.60f   // MAR 高于此值 = 打哈欠
#define MAR_YAWN_DURATION     1500    // 连续打哈欠确认时间 ms

// ─── 疲劳判断参数 ─────────────────────────────────────────────────
#define PERCLOS_WINDOW_SEC    60      // PERCLOS 统计窗口秒数
#define PERCLOS_MILD          0.15f   // 轻度疲劳阈值
#define PERCLOS_SEVERE        0.30f   // 严重疲劳阈值
#define YAWN_MILD_PER_10MIN   1       // 轻度：10分钟内打哈欠次数
#define YAWN_SEVERE_PER_10MIN 3       // 严重：10分钟内打哈欠次数

// ─── 状态机参数 ───────────────────────────────────────────────────
#define DEBOUNCE_FRAMES       5       // 防抖确认帧数
#define FATIGUE_RECOVERY_SEC  30      // FATIGUE 解除需持续正常秒数
#define RESTING_ENTER_SEC     30      // 静止多少秒进入 RESTING
#define RESTING_READY_MIN     20      // 休息多少分钟可以继续出发
#define UNKNOWN_ALARM_SEC     10      // 离岗多少秒触发报警

// ─── GPS / 行驶判断 ───────────────────────────────────────────────
#define GPS_SPEED_THRESHOLD   5.0f    // 行驶速度阈值 km/h
#define DRIVE_TIME_WARN_SEC   (3*3600 + 30*60)   // 3.5h 预警
#define DRIVE_TIME_ALARM_SEC  (4*3600)            // 4h 强制报警
#define POI_RADIUS            5000    // 高德 POI 查询半径 m

// Mock GPS 坐标（室内无信号时使用，替换为学校坐标）
#define MOCK_LAT              39.9087f
#define MOCK_LNG              116.3975f

// ─── MQ-3 酒驾阈值 ───────────────────────────────────────────────
#define MQ3_WARMUP_MS         30000   // 预热时间 ms
#define MQ3_ALARM_RAW         2048    // ADC 原始值阈值（对应 ~20mg/100mL）
#define MQ3_HYSTERESIS        100     // 滞后，防抖动

// ─── 高德 API ─────────────────────────────────────────────────────
#define AMAP_KEY              "YOUR_AMAP_KEY_HERE"
#define SERVER_URL            "http://YOUR_SERVER_IP/api/report"

// ─── 蜂鸣器频率 ──────────────────────────────────────────────────
#define BUZZ_FREQ_MILD        1000    // 轻度：1kHz
#define BUZZ_FREQ_SEVERE      2500    // 严重：2.5kHz
#define BUZZ_SHORT_MS         200     // 短鸣时长

// ─── FreeRTOS 任务栈 ─────────────────────────────────────────────
#define STACK_MAIN            8192
#define STACK_NET             4096
#define STACK_ALCOHOL         2048
