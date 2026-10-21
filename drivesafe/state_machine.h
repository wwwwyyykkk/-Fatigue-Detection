#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"

// ================================================================
//  驾盾 — 系统状态定义与共享数据结构
// ================================================================

// ─── 五状态枚举 ──────────────────────────────────────────────────
typedef enum {
    STATE_INIT    = 0,   // 初始化等待
    STATE_DRIVING = 1,   // 正常行驶检测
    STATE_RESTING = 2,   // 停车休息静默
    STATE_UNKNOWN = 3,   // 离岗异常（车动无人脸）
    STATE_FATIGUE = 4    // 疲劳报警中
} SystemState;

// ─── 疲劳等级枚举 ────────────────────────────────────────────────
typedef enum {
    FATIGUE_NORMAL = 0,
    FATIGUE_MILD   = 1,
    FATIGUE_SEVERE = 2
} FatigueLevel;

// ─── 全局共享数据（单写多读，无需互斥锁） ──────────────────────
typedef struct {
    // 当前系统状态
    volatile SystemState state;
    volatile FatigueLevel fatigueLevel;

    // AI 推理结果
    volatile float earValue;         // Eye Aspect Ratio 均值
    volatile float marValue;         // Mouth Aspect Ratio
    volatile float faceConfidence;   // 人脸置信度
    volatile bool  faceDetected;     // 当前帧是否检测到人脸

    // PERCLOS 滑动窗口（60s，5fps → 300帧）
    bool   percloseWindow[300];
    int    percloseHead;             // 环形缓冲写指针
    int    percloseCount;            // 已填充帧数
    volatile float perclosValue;     // 当前 PERCLOS（0~1）

    // 打哈欠统计（10分钟滑动窗口）
    uint32_t yawnTimestamps[50];     // 最多记录 50 次哈欠时间戳
    int      yawnHead;
    int      yawnCount;
    uint32_t yawnStartMs;            // 当次哈欠开始时间
    bool     inYawn;

    // GPS 数据
    volatile float  gpsLat;
    volatile float  gpsLng;
    volatile float  gpsSpeedKmh;
    volatile bool   gpsValid;

    // 驾驶时长
    volatile uint32_t driveStartMs;      // 本次行程开始时间
    volatile uint32_t totalDriveSec;     // 累计行驶秒数
    volatile bool     tripActive;        // 行程是否进行中

    // 酒精
    volatile bool  alcoholAlarm;
    volatile int   mq3Raw;

    // 防抖计数器（各状态转移独立维护）
    int  debounce_driving;
    int  debounce_resting;
    int  debounce_unknown;
    int  debounce_fatigue;
    int  debounce_normal;

    // FATIGUE 解除双重验证
    volatile bool    driverAck;          // 司机是否已按键
    uint32_t         recoveryStartMs;    // 开始恢复计时
    bool             inRecovery;

    // UNKNOWN 离岗计时
    uint32_t  unknownStartMs;

    // 行程事件日志（用于安全评分）
    struct TripEvent {
        uint32_t timeMs;
        uint8_t  eventType;  // 0=mild, 1=severe, 2=alcohol, 3=absent
        uint16_t durationSec;
    } events[100];
    int eventCount;

    // 互斥锁（保护 events[] 数组写入）
    SemaphoreHandle_t eventMutex;
} SharedState;

extern SharedState g;

// ─── 初始化 ──────────────────────────────────────────────────────
void state_init();

// ─── PERCLOS 操作 ─────────────────────────────────────────────────
void  perclos_push(bool eyeClosed);
void  perclos_reset();
float perclos_get();

// ─── 打哈欠操作 ──────────────────────────────────────────────────
void yawn_update(float mar);
int  yawn_count_recent(uint32_t windowMs);
void yawn_reset();

// ─── 状态机主更新函数（每帧调用） ───────────────────────────────
void state_update();

// ─── 事件记录 ────────────────────────────────────────────────────
void log_event(uint8_t type, uint16_t durationSec);

// ─── 安全评分（行程结束时调用） ──────────────────────────────────
int  calc_safety_score();
