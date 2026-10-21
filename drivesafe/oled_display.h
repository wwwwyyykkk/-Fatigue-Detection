#pragma once
// ================================================================
//  驾盾 — SSD1306 OLED 显示驱动
//  库依赖：Adafruit SSD1306 + Adafruit GFX
// ================================================================
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "state_machine.h"

#define OLED_WIDTH  128
#define OLED_HEIGHT  64
#define OLED_ADDR   0x3C

static Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

// ─── 初始化 ──────────────────────────────────────────────────────
inline bool oled_init() {
    Wire.begin(OLED_SDA, OLED_SCL);
    if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("[OLED] 初始化失败！");
        return false;
    }
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.println("DriveSafe Guardian");
    oled.println("Initializing...");
    oled.display();
    return true;
}

// ─── 显示启动画面 ─────────────────────────────────────────────────
inline void oled_splash() {
    oled.clearDisplay();
    oled.setTextSize(2);
    oled.setCursor(10, 10);
    oled.println("JIA DUAN");
    oled.setTextSize(1);
    oled.setCursor(20, 40);
    oled.println("DriveSafe Guardian");
    oled.setCursor(30, 52);
    oled.println("Initializing...");
    oled.display();
}

// ─── 显示人脸识别界面 ────────────────────────────────────────────
inline void oled_face_verify(float conf, bool passed) {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.println("== Face Verify ==");
    oled.printf("Conf: %.2f\n", conf);
    if (passed) {
        oled.setTextSize(2);
        oled.setCursor(10, 35);
        oled.println("PASSED!");
    } else {
        oled.setCursor(0, 20);
        oled.println("Scanning...");
        oled.setCursor(0, 35);
        oled.println("Look at camera");
    }
    oled.display();
}

// ─── 显示酒驾报警 ────────────────────────────────────────────────
inline void oled_alcohol_alarm(int raw) {
    oled.clearDisplay();
    oled.setTextSize(2);
    oled.setCursor(0, 0);
    oled.println("ALCOHOL");
    oled.println("ALARM!");
    oled.setTextSize(1);
    oled.printf("ADC: %d\n", raw);
    oled.println("Cannot start trip");
    oled.display();
}

// ─── 主运行状态显示（每帧刷新） ──────────────────────────────────
inline void oled_show_state() {
    oled.clearDisplay();
    oled.setTextSize(1);

    // 第1行：状态
    oled.setCursor(0, 0);
    const char* stateStr[] = {"INIT", "DRIVING", "RESTING", "ABSENT!", "FATIGUE!"};
    oled.printf("State: %s\n", stateStr[g.state]);

    // 第2行：疲劳等级（DRIVING/FATIGUE时显示）
    oled.setCursor(0, 10);
    if (g.state == STATE_DRIVING || g.state == STATE_FATIGUE) {
        const char* lvl[] = {"NORMAL", "MILD", "SEVERE"};
        oled.printf("Fatigue: %s\n", lvl[g.fatigueLevel]);
    } else if (g.state == STATE_RESTING) {
        uint32_t restSec = (millis() - g.unknownStartMs) / 1000;
        oled.printf("Rested: %lus\n", restSec);
    }

    // 第3行：EAR / MAR
    oled.setCursor(0, 20);
    oled.printf("EAR:%.2f MAR:%.2f\n", g.earValue, g.marValue);

    // 第4行：PERCLOS
    oled.setCursor(0, 30);
    oled.printf("PERCLOS: %.0f%%\n", g.perclosValue * 100.0f);

    // 第5行：速度
    oled.setCursor(0, 40);
    oled.printf("Speed: %.1fkm/h\n", g.gpsSpeedKmh);

    // 第6行：驾驶时长 / 提示
    oled.setCursor(0, 52);
    uint32_t drivMin = g.totalDriveSec / 60;
    uint32_t drivSec = g.totalDriveSec % 60;
    if (g.totalDriveSec >= DRIVE_TIME_ALARM_SEC) {
        oled.println("!STOP NOW! 4h+");
    } else if (g.totalDriveSec >= DRIVE_TIME_WARN_SEC) {
        oled.println("Rest soon! 3.5h");
    } else {
        oled.printf("Drive: %02lu:%02lu\n", drivMin, drivSec);
    }

    oled.display();
}

// ─── 显示 POI 休息点 ─────────────────────────────────────────────
inline void oled_show_poi(const char* name, float distKm) {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.println("== Rest Area Nearby ==");
    oled.setCursor(0, 16);
    oled.println(name);
    oled.setCursor(0, 30);
    oled.printf("Dist: %.1f km\n", distKm);
    oled.setCursor(0, 44);
    oled.println("Please take a rest");
    oled.display();
}

// ─── 显示最终安全评分 ────────────────────────────────────────────
inline void oled_show_score(int score) {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.println("== Trip Report ==");
    oled.setTextSize(3);
    oled.setCursor(20, 20);
    oled.printf("%d\n", score);
    oled.setTextSize(1);
    oled.setCursor(0, 52);
    if      (score >= 90) oled.println("Excellent! Drive safe");
    else if (score >= 70) oled.println("Good. Stay alert");
    else                  oled.println("Please rest more!");
    oled.display();
}
