#pragma once
// ================================================================
//  驾盾 — Quectel EG800K 驱动（GPS 解析 + 4G AT 指令）
//  UART1：TX=GPIO43, RX=GPIO44
// ================================================================
#include <Arduino.h>
#include <HardwareSerial.h>
#include "config.h"
#include "state_machine.h"

static HardwareSerial eg800k(1);   // UART1

// NMEA 行缓冲
static char  _nmeaBuf[128];
static int   _nmeaIdx = 0;

// ─── 初始化 ──────────────────────────────────────────────────────
inline void eg800k_init() {
    eg800k.begin(EG800K_BAUD, SERIAL_8N1, EG800K_RX, EG800K_TX);
    delay(2000);  // 等待模块上电
    Serial.println("[EG800K] 发送 AT 测试...");
    eg800k.println("AT");
    delay(500);
    while (eg800k.available()) Serial.write(eg800k.read());

    // 开启 GPS
    eg800k.println("AT+QGPS=1");
    delay(500);
    while (eg800k.available()) Serial.write(eg800k.read());

    // 开启 NMEA 输出（$GPRMC）
    eg800k.println("AT+QGPSCFG=\"nmeasrc\",1");
    delay(500);
    Serial.println("[EG800K] GPS 已开启，等待定位...");
}

// ─── 解析 $GPRMC 语句 ────────────────────────────────────────────
// $GPRMC,hhmmss,A,ddmm.mmmm,N,dddmm.mmmm,E,speed,course,date,,,*cs
static void parse_gprmc(const char* sentence) {
    // 复制后分割
    char buf[128];
    strncpy(buf, sentence, 127);
    buf[127] = '\0';

    char* fields[13];
    int   n = 0;
    char* p = strtok(buf, ",");
    while (p && n < 13) { fields[n++] = p; p = strtok(NULL, ","); }

    if (n < 8) return;
    if (fields[2][0] != 'A') {
        // 定位无效，使用 Mock 坐标
        if (!g.gpsValid) {
            g.gpsLat   = MOCK_LAT;
            g.gpsLng   = MOCK_LNG;
            g.gpsSpeedKmh = 0.0f;
        }
        return;
    }

    // 纬度：ddmm.mmmm → 十进制度
    float rawLat = atof(fields[3]);
    int   latDeg = (int)(rawLat / 100);
    float latMin = rawLat - latDeg * 100;
    g.gpsLat     = latDeg + latMin / 60.0f;
    if (fields[4][0] == 'S') g.gpsLat = -g.gpsLat;

    // 经度：dddmm.mmmm
    float rawLng = atof(fields[5]);
    int   lngDeg = (int)(rawLng / 100);
    float lngMin = rawLng - lngDeg * 100;
    g.gpsLng     = lngDeg + lngMin / 60.0f;
    if (fields[6][0] == 'W') g.gpsLng = -g.gpsLng;

    // 速度：节 → km/h （1节 = 1.852 km/h）
    g.gpsSpeedKmh = atof(fields[7]) * 1.852f;
    g.gpsValid    = true;
}

// ─── NMEA 逐字节解析（在主循环或中断中调用） ────────────────────
inline void eg800k_parse_tick() {
    while (eg800k.available()) {
        char c = eg800k.read();
        if (c == '$') {
            _nmeaIdx = 0;
        }
        if (_nmeaIdx < 127) {
            _nmeaBuf[_nmeaIdx++] = c;
            _nmeaBuf[_nmeaIdx]   = '\0';
        }
        if (c == '\n' && _nmeaIdx > 6) {
            if (strncmp(_nmeaBuf, "$GPRMC", 6) == 0 ||
                strncmp(_nmeaBuf, "$GNRMC", 6) == 0) {
                parse_gprmc(_nmeaBuf);
            }
            _nmeaIdx = 0;
        }
    }
}

// ─── 发送 HTTP GET 请求（通过 AT 指令） ──────────────────────────
inline bool eg800k_http_get(const char* url, char* respBuf, int bufLen) {
    eg800k.println("AT+QHTTPURL=" + String(strlen(url)) + ",80");
    delay(200);
    eg800k.print(url);
    delay(1000);
    eg800k.println("AT+QHTTPGET=80");
    delay(3000);

    // 读取响应
    int idx = 0;
    uint32_t t = millis();
    while ((millis() - t) < 5000 && idx < bufLen - 1) {
        if (eg800k.available()) {
            respBuf[idx++] = eg800k.read();
        }
    }
    respBuf[idx] = '\0';
    return (idx > 0);
}

// ─── 上报数据到服务器 ────────────────────────────────────────────
inline void eg800k_report(const char* jsonPayload) {
    // 简化 POST：使用 AT+QHTTPOST
    char url[256];
    snprintf(url, sizeof(url), SERVER_URL);

    // 设置 URL
    eg800k.printf("AT+QHTTPURL=%d,80\r\n", strlen(url));
    delay(300);
    eg800k.print(url);
    delay(500);

    // POST
    eg800k.printf("AT+QHTTPPOST=%d,80,80\r\n", strlen(jsonPayload));
    delay(500);
    eg800k.print(jsonPayload);
    delay(2000);
}

// ─── 查询高德 POI（附近停车场/服务区） ──────────────────────────
inline void eg800k_query_poi(float lat, float lng) {
    char url[256];
    snprintf(url, sizeof(url),
        "http://restapi.amap.com/v3/place/around"
        "?key=%s&location=%.6f,%.6f&keywords=服务区|停车场&radius=%d&output=json&offset=1",
        AMAP_KEY, lng, lat, POI_RADIUS);

    char resp[512];
    if (eg800k_http_get(url, resp, sizeof(resp))) {
        // 简单解析：找 "name":"..." 字段
        char* nameStart = strstr(resp, "\"name\":\"");
        if (nameStart) {
            nameStart += 8;
            char* nameEnd = strchr(nameStart, '"');
            if (nameEnd) {
                char poiName[64];
                int  len = min((int)(nameEnd - nameStart), 63);
                strncpy(poiName, nameStart, len);
                poiName[len] = '\0';

                // 距离（简化：用 distance 字段）
                float dist = 2.5f;  // 默认值
                char* distStart = strstr(resp, "\"distance\":\"");
                if (distStart) {
                    distStart += 12;
                    dist = atof(distStart) / 1000.0f;
                }

                extern void oled_show_poi(const char*, float);
                oled_show_poi(poiName, dist);
            }
        }
    }
}
