#pragma once
// ================================================================
//  驾盾 — MQ-3 酒精传感器驱动 + FreeRTOS 采样任务
// ================================================================
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include "config.h"
#include "state_machine.h"

// 事件标志组（通知主 Task）
static EventGroupHandle_t _alcoholEvents = NULL;
#define ALCOHOL_ALARM_BIT   (1 << 0)
#define ALCOHOL_CLEAR_BIT   (1 << 1)

// 滞后状态（防止反复触发）
static bool _prevAlarm = false;

// ─── 初始化 ──────────────────────────────────────────────────────
inline void mq3_init() {
    pinMode(MQ3_PIN, INPUT);
    _alcoholEvents = xEventGroupCreate();
    Serial.printf("[MQ-3] 预热中，请等待 %d 秒...\n", MQ3_WARMUP_MS / 1000);
}

// ─── 采样任务（独立 FreeRTOS Task，优先级 3，每 1 秒采样） ─────
static void mq3_task(void* arg) {
    // 等待预热完成
    vTaskDelay(pdMS_TO_TICKS(MQ3_WARMUP_MS));
    Serial.println("[MQ-3] 预热完成，开始采样");

    while (true) {
        int raw = analogRead(MQ3_PIN);
        g.mq3Raw = raw;

        bool alarm = (raw > MQ3_ALARM_RAW);

        if (alarm && !_prevAlarm) {
            // 上升沿：触发报警
            g.alcoholAlarm = true;
            xEventGroupSetBits(_alcoholEvents, ALCOHOL_ALARM_BIT);
            Serial.printf("[MQ-3] 酒精超标！ADC=%d\n", raw);
            _prevAlarm = true;
        } else if (!alarm && _prevAlarm && (raw < MQ3_ALARM_RAW - MQ3_HYSTERESIS)) {
            // 下降沿（带滞后）：解除报警
            g.alcoholAlarm = false;
            xEventGroupSetBits(_alcoholEvents, ALCOHOL_CLEAR_BIT);
            Serial.printf("[MQ-3] 酒精恢复正常，ADC=%d\n", raw);
            _prevAlarm = false;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ─── 启动采样任务 ─────────────────────────────────────────────────
inline void mq3_start_task() {
    xTaskCreate(mq3_task, "MQ3Task", STACK_ALCOHOL, NULL, 3, NULL);
}

// ─── 阻塞等待预检通过（启动时使用） ─────────────────────────────
// 返回 true=通过，false=超时或酒精超标
inline bool mq3_precheck(uint32_t timeoutMs) {
    // 先等预热
    uint32_t start = millis();
    Serial.print("[MQ-3] 等待预热");
    while ((millis() - start) < MQ3_WARMUP_MS) {
        Serial.print(".");
        delay(1000);
    }
    Serial.println();

    int raw = analogRead(MQ3_PIN);
    g.mq3Raw = raw;
    Serial.printf("[MQ-3] 预检 ADC=%d 阈值=%d\n", raw, MQ3_ALARM_RAW);

    if (raw > MQ3_ALARM_RAW) {
        g.alcoholAlarm = true;
        return false;
    }
    g.alcoholAlarm = false;
    return true;
}
