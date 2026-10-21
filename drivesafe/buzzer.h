#pragma once
// ================================================================
//  驾盾 — 无源蜂鸣器驱动（PWM）
// ================================================================
#include <Arduino.h>
#include "config.h"
#include "state_machine.h"

#define BUZZER_CHANNEL  0    // LEDC 通道（避开摄像头用的 0，这里用 1）
#undef  BUZZER_CHANNEL
#define BUZZER_CHANNEL  1

static bool  _buzzerActive   = false;
static uint8_t _buzzerMode   = 0;   // 0=off, 1=mild短鸣, 2=severe持续
static uint32_t _buzzLastMs  = 0;

// ─── 初始化 ──────────────────────────────────────────────────────
inline void buzzer_init() {
    ledcSetup(BUZZER_CHANNEL, BUZZ_FREQ_MILD, 8);
    ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
    ledcWrite(BUZZER_CHANNEL, 0);
}

// ─── 停止 ────────────────────────────────────────────────────────
inline void buzzer_stop() {
    ledcWrite(BUZZER_CHANNEL, 0);
    _buzzerActive = false;
    _buzzerMode   = 0;
}

// ─── 触发报警（非阻塞，由 buzzer_tick 执行） ─────────────────────
inline void buzzer_alarm(FatigueLevel level) {
    if (level == FATIGUE_MILD) {
        _buzzerMode = 1;
    } else if (level == FATIGUE_SEVERE) {
        _buzzerMode = 2;
        ledcSetup(BUZZER_CHANNEL, BUZZ_FREQ_SEVERE, 8);
        ledcWrite(BUZZER_CHANNEL, 128);  // 50% 占空比
        _buzzerActive = true;
    }
    _buzzLastMs = millis();
}

// ─── 每帧调用，处理非阻塞鸣叫逻辑 ───────────────────────────────
inline void buzzer_tick() {
    uint32_t now = millis();

    if (_buzzerMode == 1) {
        // 轻度：短鸣一次，BUZZ_SHORT_MS 后停
        if (!_buzzerActive) {
            ledcSetup(BUZZER_CHANNEL, BUZZ_FREQ_MILD, 8);
            ledcWrite(BUZZER_CHANNEL, 128);
            _buzzerActive = true;
            _buzzLastMs   = now;
        } else if ((now - _buzzLastMs) >= BUZZ_SHORT_MS) {
            ledcWrite(BUZZER_CHANNEL, 0);
            _buzzerActive = false;
            _buzzerMode   = 0;
        }
    }
    // severe 模式持续鸣响，由 buzzer_stop() 停止
}
