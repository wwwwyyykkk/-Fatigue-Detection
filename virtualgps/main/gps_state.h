/**
 * gps_state.h - 虚拟GPS状态管理
 *
 * 维护全局状态结构体，提供线程安全的状态更新接口
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

/* ========== 地点类型枚举 ========== */
typedef enum {
    LOCATION_NORMAL = 0,        // 普通路段，不可停车休息
    LOCATION_SERVICE_AREA = 1,  // 服务区，可以停车休息
    LOCATION_EMERGENCY_LANE = 2 // 应急车道，禁止停车（违规）
} location_type_t;

/* ========== 全局状态结构体 ========== */
typedef struct {
    float speed_kmh;                // 当前速度 (km/h)
    location_type_t location;       // 当前地点类型
    int driver_id;                  // 司机ID (1 或 2)
    int drive_seconds;              // 累计驾驶时长 (秒，自动递增)
    float gps_lat;                  // 纬度
    float gps_lng;                  // 经度
    bool  use_real_gps;             // true=真实GPS坐标, false=虚拟GPS坐标
    bool  has_gps_fix;              // true=GPS模块有有效定位
    SemaphoreHandle_t mutex;        // 互斥锁 (保护并发访问)
} virtual_gps_state_t;

/* ========== 事件组定义 ========== */
#define STATE_CHANGED_BIT   (1 << 0)    // 状态变化事件位

/* ========== 全局变量声明 ========== */
extern virtual_gps_state_t g_gps_state;
extern EventGroupHandle_t g_state_event;

/* ========== 初始化 ========== */
void gps_state_init(void);

/* ========== 状态读取 (线程安全) ========== */
void gps_state_read(virtual_gps_state_t *out);

/* ========== 状态更新接口 (线程安全，更新后触发 STATE_CHANGED_BIT) ========== */
void gps_state_set_speed(float speed_kmh);
int  gps_state_cycle_location(void);         // 循环切换地点, 返回新地点(0/1/2)
void gps_state_set_location(int loc);        // 直接设置地点 0/1/2 (非循环)
void gps_state_toggle_driver(void);          // 司机切换: 1<->2
void gps_state_set_driver(int driver_id);    // 直接设置司机 1/2
int  gps_state_add_drive_time(int minutes);  // 累加驾驶时长, 返回总分钟数
void gps_state_tick_second(void);           // 每秒递增驾驶时间
void gps_state_set_gps(float lat, float lng); // 设置GPS坐标
bool gps_state_is_real_gps(void);             // 查询GPS来源 (true=真实)
bool gps_state_toggle_gps_source(void);       // 切换真实↔虚拟, 返回新值
void gps_state_set_fix_status(bool has_fix);  // 更新GPS信号状态

/* ========== 辅助函数 ========== */
const char *location_type_str(location_type_t loc);
