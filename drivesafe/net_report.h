#pragma once
// ================================================================
//  驾盾 — 网络上报任务（FreeRTOS Task，优先级 2）
//  每 5 秒上报一次 GPS/状态数据，事件触发时立即上报
// ================================================================
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "config.h"
#include "state_machine.h"
#include "eg800k_gps.h"

// 上次上报时间
static uint32_t _lastReportMs = 0;
static bool     _poiSent      = false;   // 3.5h POI 是否已推送
static bool     _alarmSent    = false;   // 4h 强制报警是否已发送

// ─── 构造 JSON 上报包 ─────────────────────────────────────────────
static void build_json(char* buf, int bufLen) {
    const char* stateStr[] = {"INIT","DRIVING","RESTING","UNKNOWN","FATIGUE"};
    const char* lvlStr[]   = {"NORMAL","MILD","SEVERE"};
    snprintf(buf, bufLen,
        "{\"lat\":%.6f,\"lng\":%.6f,\"speed\":%.1f,"
        "\"state\":\"%s\",\"fatigue\":\"%s\","
        "\"perclos\":%.3f,\"ear\":%.3f,\"mar\":%.3f,"
        "\"alcohol\":%s,\"drive_sec\":%lu,\"score\":-1}",
        g.gpsLat, g.gpsLng, g.gpsSpeedKmh,
        stateStr[g.state], lvlStr[g.fatigueLevel],
        g.perclosValue, g.earValue, g.marValue,
        g.alcoholAlarm ? "true" : "false",
        g.totalDriveSec
    );
}

// ─── 网络上报任务主体 ─────────────────────────────────────────────
static void net_task(void* arg) {
    char jsonBuf[512];

    while (true) {
        uint32_t now = millis();

        // 每 5 秒上报一次
        if ((now - _lastReportMs) >= 5000 && g.tripActive) {
            build_json(jsonBuf, sizeof(jsonBuf));
            eg800k_report(jsonBuf);
            _lastReportMs = now;
        }

        // 3.5h 预警：查询并推送 POI
        if (!_poiSent && g.totalDriveSec >= DRIVE_TIME_WARN_SEC && g.tripActive) {
            Serial.println("[NET] 驾驶时间达 3.5h，查询附近休息点...");
            eg800k_query_poi(g.gpsLat, g.gpsLng);
            _poiSent = true;
        }

        // 4h 强制报警：上报并持续鸣响（蜂鸣器由主 Task 处理，这里只上报）
        if (!_alarmSent && g.totalDriveSec >= DRIVE_TIME_ALARM_SEC && g.tripActive) {
            Serial.println("[NET] 驾驶时间达 4h！强制上报报警");
            snprintf(jsonBuf, sizeof(jsonBuf),
                "{\"alert\":\"DRIVE_4H\",\"lat\":%.6f,\"lng\":%.6f}",
                g.gpsLat, g.gpsLng);
            eg800k_report(jsonBuf);
            _alarmSent = true;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ─── 启动网络任务 ─────────────────────────────────────────────────
inline void net_start_task() {
    xTaskCreate(net_task, "NetTask", STACK_NET, NULL, 2, NULL);
}

// ─── 上报行程报告（行程结束时调用） ──────────────────────────────
inline void net_send_trip_report(int score) {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"trip_end\":true,\"score\":%d,\"drive_sec\":%lu,"
        "\"events\":%d,\"lat\":%.6f,\"lng\":%.6f}",
        score, g.totalDriveSec, g.eventCount, g.gpsLat, g.gpsLng);
    eg800k_report(buf);
    Serial.printf("[NET] 行程报告已上报，评分: %d\n", score);
}

// ─── 重置（新行程开始时调用） ────────────────────────────────────
inline void net_reset_trip() {
    _lastReportMs = 0;
    _poiSent      = false;
    _alarmSent    = false;
}
