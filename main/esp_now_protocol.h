/**
 * esp_now_protocol.h - ESP-NOW 板间通信协议 (主控板 ↔ VirtualGPS)
 *
 * 替换原有 UART2 有线通信，通过 WiFi ESP-NOW 无线传输
 *
 * cmd 0x10: VirtualGPS → 主控板 (替代 G-prefix)
 *   gps_data_msg_t: speed + location + driver_id + drive_sec + lat + lng (18 bytes)
 *
 * cmd 0x20: 主控板 → VirtualGPS (替代 F-prefix)
 *   fatigue_state_msg_t: fatigue_level + face_verified + alcohol + driver_confirmed + sys_status (5 bytes)
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 指令码 ── */
#define ESP_NOW_CMD_GPS_DATA    0x10  /* VirtualGPS → Main: GPS/driving state */
#define ESP_NOW_CMD_FATIGUE     0x20  /* Main → VirtualGPS: fatigue/face/sys status */
#define ESP_NOW_CMD_REST        0x30  /* VirtualGPS → Main: rest request (1 byte: 1=rest) */
#define ESP_NOW_CMD_MOCK_STATE  0x50  /* VirtualGPS → Main: Rainmaker mock state (覆盖上报值) */
#define ESP_NOW_CMD_DEMO        0x51  /* VirtualGPS → Main: Demo Mode ramp (1=start, 0=stop) */

/* ── Payload: VirtualGPS → 主控板, Rainmaker mock 数据 (覆盖上报值, 不改真实检测) ── */
typedef struct __attribute__((packed)) {
    uint8_t weather_risk;      /* 0=晴天 1=雨雪 2=雾霾 */
    int16_t perclos;           /* -1=不覆盖(用主板真实值), 0-100=覆盖上报给 EG800K 的 perclos */
    uint8_t fatigue_level;     /* 0=NORMAL 1=MILD 2=SEVERE */
    uint8_t alcohol;           /* 0=未检出 1=检测到 */
    uint8_t face_verified;     /* 0=未验证 1=已验证 */
    uint8_t reserved;          /* 保留 */
} mock_state_msg_t;

/* ── Payload: VirtualGPS → 主控板 (19 bytes packed) ── */
typedef struct __attribute__((packed)) {
    float    speed;        /* km/h */
    uint8_t  location;     /* 0=normal, 1=service, 2=emergency */
    uint8_t  driver_id;    /* 1 or 2 */
    int32_t  drive_sec;    /* total driving seconds */
    float    lat;          /* latitude (degrees) */
    float    lng;          /* longitude (degrees) */
    uint8_t  gps_mode;     /* 0=真实GPS, 1=模拟mock GPS */
} gps_data_msg_t;

/* ── identify_event 取值 ── */
#define IDENTIFY_EVENT_NONE       0  /* 无事件 */
#define IDENTIFY_EVENT_START      1  /* 开始身份验证 */
#define IDENTIFY_EVENT_SUCCESS    2  /* 验证成功 */
#define IDENTIFY_EVENT_FAIL       3  /* 验证失败（陌生人/超时） */
#define IDENTIFY_EVENT_REST_START 4  /* 休息模式已开启 */
#define IDENTIFY_EVENT_REST_DONE  5  /* 休息完成 */

/* ── Payload: 主控板 → VirtualGPS (9 bytes packed) ── */
typedef struct __attribute__((packed)) {
    uint8_t fatigue_level;     /* 0=NORMAL, 1=MILD, 2=SEVERE */
    uint8_t face_verified;     /* 0=unverified, 1=verified */
    uint8_t alcohol;           /* 0=normal, 1=over_limit */
    uint8_t driver_confirmed;  /* confirmed driver ID */
    uint8_t sys_status;        /* 0=init, 1=running, 2=error */
    uint8_t identify_event;    /* IDENTIFY_EVENT_NONE/START/SUCCESS/FAIL */
    int16_t pred_minutes;      /* 预测: 还有几分钟到 pred_level (0/负=无预测) */
    uint8_t pred_level;        /* 预测目标等级: 1=轻度 2=严重 */
} fatigue_state_msg_t;

#ifdef __cplusplus
}
#endif
