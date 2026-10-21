#include "state_machine.h"
#include "buzzer.h"
#include "oled_display.h"
#include <string.h>

// ================================================================
//  驾盾 — 状态机实现
// ================================================================

SharedState g;

// ─── 初始化 ──────────────────────────────────────────────────────
void state_init() {
    memset(&g, 0, sizeof(g));
    g.state        = STATE_INIT;
    g.fatigueLevel = FATIGUE_NORMAL;
    g.gpsLat       = MOCK_LAT;
    g.gpsLng       = MOCK_LNG;
    g.eventMutex   = xSemaphoreCreateMutex();
}

// ─── PERCLOS 滑动窗口（环形缓冲，300帧@5fps=60s） ──────────────
void perclos_push(bool eyeClosed) {
    g.percloseWindow[g.percloseHead] = eyeClosed;
    g.percloseHead = (g.percloseHead + 1) % 300;
    if (g.percloseCount < 300) g.percloseCount++;

    // 统计窗口内闭眼帧数
    int closed = 0;
    for (int i = 0; i < g.percloseCount; i++) {
        if (g.percloseWindow[i]) closed++;
    }
    g.perclosValue = (float)closed / (float)g.percloseCount;
}

void perclos_reset() {
    memset(g.percloseWindow, 0, sizeof(g.percloseWindow));
    g.percloseHead  = 0;
    g.percloseCount = 0;
    g.perclosValue  = 0.0f;
}

float perclos_get() {
    return g.perclosValue;
}

// ─── 打哈欠统计（10分钟滑动窗口） ────────────────────────────────
void yawn_update(float mar) {
    uint32_t now = millis();

    if (mar > MAR_YAWN_THRESHOLD) {
        if (!g.inYawn) {
            g.inYawn      = true;
            g.yawnStartMs = now;
        } else if ((now - g.yawnStartMs) >= MAR_YAWN_DURATION) {
            // 确认一次完整打哈欠
            g.yawnTimestamps[g.yawnHead] = now;
            g.yawnHead = (g.yawnHead + 1) % 50;
            if (g.yawnCount < 50) g.yawnCount++;
            g.inYawn = false;  // 重置，等待下次
        }
    } else {
        g.inYawn = false;
    }
}

int yawn_count_recent(uint32_t windowMs) {
    uint32_t now = millis();
    int cnt = 0;
    for (int i = 0; i < g.yawnCount; i++) {
        if ((now - g.yawnTimestamps[i]) <= windowMs) cnt++;
    }
    return cnt;
}

void yawn_reset() {
    memset(g.yawnTimestamps, 0, sizeof(g.yawnTimestamps));
    g.yawnHead  = 0;
    g.yawnCount = 0;
    g.inYawn    = false;
}

// ─── 事件记录 ────────────────────────────────────────────────────
void log_event(uint8_t type, uint16_t durationSec) {
    if (xSemaphoreTake(g.eventMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (g.eventCount < 100) {
            g.events[g.eventCount].timeMs      = millis();
            g.events[g.eventCount].eventType   = type;
            g.events[g.eventCount].durationSec = durationSec;
            g.eventCount++;
        }
        xSemaphoreGive(g.eventMutex);
    }
}

// ─── 安全评分（0~100） ────────────────────────────────────────────
int calc_safety_score() {
    int score = 100;

    // 扣分规则
    for (int i = 0; i < g.eventCount; i++) {
        switch (g.events[i].eventType) {
            case 0: score -= 3;  break;  // MILD 疲劳，每次-3
            case 1: score -= 8;  break;  // SEVERE 疲劳，每次-8
            case 2: score -= 30; break;  // 酒精报警，-30
            case 3: score -= 5;  break;  // 离岗报警，每次-5
        }
        // 持续时间附加扣分
        score -= (g.events[i].durationSec / 30);
    }

    // 未按时休息扣分
    if (g.totalDriveSec > DRIVE_TIME_ALARM_SEC) {
        score -= 15;
    } else if (g.totalDriveSec > DRIVE_TIME_WARN_SEC) {
        score -= 5;
    }

    return max(0, min(100, score));
}

// ─── 状态机核心更新（每帧调用，约200ms一次） ─────────────────────
void state_update() {
    bool eyeClosed   = (g.earValue < EAR_CLOSED_THRESHOLD);
    bool face        = g.faceDetected;
    bool moving      = (g.gpsSpeedKmh > GPS_SPEED_THRESHOLD);
    bool alcohol     = g.alcoholAlarm;
    float perclos    = perclos_get();
    int  yawnRecent  = yawn_count_recent(10UL * 60 * 1000);  // 10分钟窗口
    uint32_t now     = millis();

    // 酒精报警优先级最高，直接触发 FATIGUE
    if (alcohol && g.state != STATE_FATIGUE) {
        g.state = STATE_FATIGUE;
        g.fatigueLevel = FATIGUE_SEVERE;
        log_event(2, 0);
        buzzer_alarm(FATIGUE_SEVERE);
        oled_show_state();
        // 清空防抖计数器
        g.debounce_driving = g.debounce_resting = 0;
        g.debounce_unknown = g.debounce_fatigue = 0;
        return;
    }

    switch (g.state) {

    // ── INIT ─────────────────────────────────────────────────────
    case STATE_INIT:
        // 等待 15 帧数据稳定后，直接进入 DRIVING 等待人脸
        if (g.percloseCount >= 15) {
            g.state = STATE_DRIVING;
            perclos_reset();
            yawn_reset();
            g.driveStartMs = now;
        }
        break;

    // ── DRIVING ──────────────────────────────────────────────────
    case STATE_DRIVING: {
        // 累计驾驶时间
        if (g.tripActive) {
            g.totalDriveSec = (now - g.driveStartMs) / 1000;
        }

        // → RESTING：不动超过阈值
        if (!moving) {
            g.debounce_resting++;
            g.debounce_driving = 0;
            if (g.debounce_resting >= DEBOUNCE_FRAMES) {
                if (g.unknownStartMs == 0) g.unknownStartMs = now;
                // 需要持续 RESTING_ENTER_SEC 秒
                if ((now - g.unknownStartMs) >= (uint32_t)(RESTING_ENTER_SEC * 1000)) {
                    g.state = STATE_RESTING;
                    perclos_reset();
                    yawn_reset();
                    g.debounce_resting = 0;
                    g.unknownStartMs   = 0;
                    oled_show_state();
                }
            }
        } else {
            g.debounce_resting = 0;
            g.unknownStartMs   = 0;
        }

        // → UNKNOWN：在动但无人脸
        if (moving && !face) {
            g.debounce_unknown++;
            if (g.debounce_unknown >= DEBOUNCE_FRAMES) {
                g.state = STATE_UNKNOWN;
                g.unknownStartMs = now;
                g.debounce_unknown = 0;
                oled_show_state();
            }
        } else {
            g.debounce_unknown = 0;
        }

        // → FATIGUE：疲劳指标超阈值
        bool fatigueTrigger = (perclos > PERCLOS_SEVERE) ||
                              (yawnRecent >= YAWN_SEVERE_PER_10MIN);
        bool mildTrigger    = (perclos > PERCLOS_MILD) ||
                              (yawnRecent >= YAWN_MILD_PER_10MIN);

        if (fatigueTrigger) {
            g.debounce_fatigue++;
            if (g.debounce_fatigue >= DEBOUNCE_FRAMES) {
                g.state        = STATE_FATIGUE;
                g.fatigueLevel = FATIGUE_SEVERE;
                g.debounce_fatigue = 0;
                log_event(1, 0);
                buzzer_alarm(FATIGUE_SEVERE);
                oled_show_state();
            }
        } else if (mildTrigger) {
            g.fatigueLevel = FATIGUE_MILD;
            buzzer_alarm(FATIGUE_MILD);
            oled_show_state();
        } else {
            g.debounce_fatigue = 0;
            g.fatigueLevel     = FATIGUE_NORMAL;
        }

        // 更新 PERCLOS
        perclos_push(eyeClosed);
        yawn_update(g.marValue);
        break;
    }

    // ── RESTING ──────────────────────────────────────────────────
    case STATE_RESTING:
        // → DRIVING：重新开动且有人脸
        if (moving && face) {
            g.debounce_driving++;
            if (g.debounce_driving >= DEBOUNCE_FRAMES) {
                g.state = STATE_DRIVING;
                perclos_reset();
                yawn_reset();
                g.driveStartMs     = now;
                g.debounce_driving = 0;
                oled_show_state();
            }
        } else {
            g.debounce_driving = 0;
        }
        break;

    // ── UNKNOWN（离岗） ───────────────────────────────────────────
    case STATE_UNKNOWN:
        // → DRIVING：人脸重现
        if (face) {
            g.debounce_driving++;
            if (g.debounce_driving >= DEBOUNCE_FRAMES) {
                g.state = STATE_DRIVING;
                g.debounce_driving = 0;
                oled_show_state();
            }
        } else {
            g.debounce_driving = 0;
        }
        // → RESTING：停车
        if (!moving) {
            g.debounce_resting++;
            if (g.debounce_resting >= DEBOUNCE_FRAMES) {
                g.state = STATE_RESTING;
                g.debounce_resting = 0;
                oled_show_state();
            }
        } else {
            g.debounce_resting = 0;
        }
        // 离岗超时报警
        if ((now - g.unknownStartMs) >= (uint32_t)(UNKNOWN_ALARM_SEC * 1000)) {
            buzzer_alarm(FATIGUE_SEVERE);
            log_event(3, (now - g.unknownStartMs) / 1000);
            g.unknownStartMs = now;  // 重置，避免重复记录
        }
        break;

    // ── FATIGUE ───────────────────────────────────────────────────
    case STATE_FATIGUE:
        // 解除双重验证：持续 30s 正常 AND 司机按键
        bool nowNormal = (perclos < PERCLOS_MILD) && (yawnRecent < YAWN_MILD_PER_10MIN);

        if (nowNormal && !g.inRecovery) {
            g.inRecovery      = true;
            g.recoveryStartMs = now;
        } else if (!nowNormal) {
            g.inRecovery = false;
            g.driverAck  = false;
        }

        bool recoveryTimeOk = g.inRecovery &&
            ((now - g.recoveryStartMs) >= (uint32_t)(FATIGUE_RECOVERY_SEC * 1000));

        if (recoveryTimeOk && g.driverAck) {
            g.state       = STATE_DRIVING;
            g.fatigueLevel = FATIGUE_NORMAL;
            g.inRecovery  = false;
            g.driverAck   = false;
            perclos_reset();
            yawn_reset();
            buzzer_stop();
            oled_show_state();
        }

        // 停车也可解除
        if (!moving) {
            g.debounce_resting++;
            if (g.debounce_resting >= DEBOUNCE_FRAMES) {
                g.state = STATE_RESTING;
                g.fatigueLevel = FATIGUE_NORMAL;
                g.debounce_resting = 0;
                g.inRecovery = false;
                buzzer_stop();
                perclos_reset();
                oled_show_state();
            }
        } else {
            g.debounce_resting = 0;
        }

        perclos_push(eyeClosed);
        yawn_update(g.marValue);
        break;
    }
}
