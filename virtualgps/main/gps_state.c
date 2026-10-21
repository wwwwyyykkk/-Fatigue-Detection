/**
 * gps_state.c - 虚拟GPS状态管理实现
 */
#include "gps_state.h"
#include "config.h"
#include "esp_log.h"
#include <string.h>
#include <math.h>

static const char *TAG = "GPS_STATE";

/* 全局状态 */
virtual_gps_state_t g_gps_state;
EventGroupHandle_t g_state_event = NULL;

/* ========== 初始化 ========== */
void gps_state_init(void)
{
    memset(&g_gps_state, 0, sizeof(g_gps_state));

    g_gps_state.speed_kmh = DEFAULT_SPEED_KMH;
    g_gps_state.location = DEFAULT_LOCATION;
    g_gps_state.driver_id = DEFAULT_DRIVER_ID;
    g_gps_state.drive_seconds = 0;
    g_gps_state.gps_lat = DEFAULT_GPS_LAT;
    g_gps_state.gps_lng = DEFAULT_GPS_LNG;
    g_gps_state.use_real_gps = false; // 默认模拟(mock)——未接真实 GPS 模块, 用模拟坐标
    g_gps_state.has_gps_fix  = false; // 初始无信号

    g_gps_state.mutex = xSemaphoreCreateMutex();
    assert(g_gps_state.mutex != NULL);

    g_state_event = xEventGroupCreate();
    assert(g_state_event != NULL);

    ESP_LOGI(TAG, "GPS state initialized: speed=%.0f, loc=%s, driver=%d, GPS=%.4f,%.4f",
             g_gps_state.speed_kmh,
             location_type_str(g_gps_state.location),
             g_gps_state.driver_id,
             g_gps_state.gps_lat,
             g_gps_state.gps_lng);
}

/* ========== 状态读取 ========== */
void gps_state_read(virtual_gps_state_t *out)
{
    xSemaphoreTake(g_gps_state.mutex, portMAX_DELAY);
    memcpy(out, &g_gps_state, sizeof(virtual_gps_state_t));
    xSemaphoreGive(g_gps_state.mutex);
}

/* ========== 设置速度 ========== */
void gps_state_set_speed(float speed_kmh)
{
    xSemaphoreTake(g_gps_state.mutex, portMAX_DELAY);
    g_gps_state.speed_kmh = speed_kmh;
    xSemaphoreGive(g_gps_state.mutex);

    ESP_LOGI(TAG, "Speed set to %.0f km/h", speed_kmh);
    xEventGroupSetBits(g_state_event, STATE_CHANGED_BIT);
}

/* ========== 循环切换地点 ========== */
int gps_state_cycle_location(void)
{
    xSemaphoreTake(g_gps_state.mutex, portMAX_DELAY);
    g_gps_state.location = (location_type_t)((g_gps_state.location + 1) % 3);
    int new_loc = (int)g_gps_state.location;
    xSemaphoreGive(g_gps_state.mutex);

    ESP_LOGI(TAG, "Location changed to: %s", location_type_str((location_type_t)new_loc));
    xEventGroupSetBits(g_state_event, STATE_CHANGED_BIT);
    return new_loc;
}

/* ========== 切换司机（重置连续驾驶计时） ========== */
void gps_state_toggle_driver(void)
{
    xSemaphoreTake(g_gps_state.mutex, portMAX_DELAY);
    g_gps_state.driver_id = (g_gps_state.driver_id == 1) ? 2 : 1;
    g_gps_state.drive_seconds = 0;
    int new_id = g_gps_state.driver_id;
    xSemaphoreGive(g_gps_state.mutex);

    ESP_LOGI(TAG, "Driver switched to: #%d (drive time reset)", new_id);
    xEventGroupSetBits(g_state_event, STATE_CHANGED_BIT);
}

/* ========== 直接设置地点 ========== */
void gps_state_set_location(int loc)
{
    if (loc < 0 || loc > 2) return;
    xSemaphoreTake(g_gps_state.mutex, portMAX_DELAY);
    g_gps_state.location = (location_type_t)loc;
    xSemaphoreGive(g_gps_state.mutex);

    ESP_LOGI(TAG, "Location set to: %s", location_type_str((location_type_t)loc));
    xEventGroupSetBits(g_state_event, STATE_CHANGED_BIT);
}

/* ========== 直接设置司机（重置连续驾驶计时） ========== */
void gps_state_set_driver(int driver_id)
{
    if (driver_id < 1 || driver_id > 2) return;
    xSemaphoreTake(g_gps_state.mutex, portMAX_DELAY);
    g_gps_state.driver_id = driver_id;
    g_gps_state.drive_seconds = 0;
    xSemaphoreGive(g_gps_state.mutex);

    ESP_LOGI(TAG, "Driver set to: #%d (drive time reset)", driver_id);
    xEventGroupSetBits(g_state_event, STATE_CHANGED_BIT);
}

/* ========== 累加驾驶时长（分钟） ========== */
int gps_state_add_drive_time(int minutes)
{
    xSemaphoreTake(g_gps_state.mutex, portMAX_DELAY);
    g_gps_state.drive_seconds += minutes * 60;
    int new_sec = g_gps_state.drive_seconds;
    xSemaphoreGive(g_gps_state.mutex);

    ESP_LOGI(TAG, "Drive time +%dmin, total: %d:%02d:%02d",
             minutes, new_sec / 3600, (new_sec % 3600) / 60, new_sec % 60);
    xEventGroupSetBits(g_state_event, STATE_CHANGED_BIT);
    return new_sec / 60;
}

/* ========== 每秒递增驾驶时间 ========== */
void gps_state_tick_second(void)
{
    xSemaphoreTake(g_gps_state.mutex, portMAX_DELAY);
    g_gps_state.drive_seconds++;
    xSemaphoreGive(g_gps_state.mutex);

    // 不每秒钟都触发 OLED 刷新（太频繁），改为每 10 秒刷新一次
    // OLED 刷新任务通过定时器也能醒来
}

/* ========== 设置GPS坐标 ========== */
void gps_state_set_gps(float lat, float lng)
{
    xSemaphoreTake(g_gps_state.mutex, portMAX_DELAY);
    g_gps_state.gps_lat = lat;
    g_gps_state.gps_lng = lng;
    xSemaphoreGive(g_gps_state.mutex);

    xEventGroupSetBits(g_state_event, STATE_CHANGED_BIT);
}

/* ========== 查询GPS来源 ========== */
bool gps_state_is_real_gps(void)
{
    xSemaphoreTake(g_gps_state.mutex, portMAX_DELAY);
    bool real = g_gps_state.use_real_gps;
    xSemaphoreGive(g_gps_state.mutex);
    return real;
}

/* ========== 切换真实↔虚拟GPS ========== */
bool gps_state_toggle_gps_source(void)
{
    xSemaphoreTake(g_gps_state.mutex, portMAX_DELAY);
    g_gps_state.use_real_gps = !g_gps_state.use_real_gps;
    bool now_real = g_gps_state.use_real_gps;
    xSemaphoreGive(g_gps_state.mutex);

    ESP_LOGI(TAG, "GPS source: %s", now_real ? "REAL" : "VIRTUAL");
    xEventGroupSetBits(g_state_event, STATE_CHANGED_BIT);
    return now_real;
}

/* ========== 更新GPS信号状态 ========== */
void gps_state_set_fix_status(bool has_fix)
{
    xSemaphoreTake(g_gps_state.mutex, portMAX_DELAY);
    bool changed = (g_gps_state.has_gps_fix != has_fix);
    g_gps_state.has_gps_fix = has_fix;
    xSemaphoreGive(g_gps_state.mutex);
    if (changed) {
        ESP_LOGI(TAG, "GPS fix: %s", has_fix ? "ACQUIRED ✅" : "LOST ❌");
        xEventGroupSetBits(g_state_event, STATE_CHANGED_BIT);
    }
}

/* ========== 地点类型转字符串 ========== */
const char *location_type_str(location_type_t loc)
{
    switch (loc) {
        case LOCATION_NORMAL:        return "Normal Road";
        case LOCATION_SERVICE_AREA:  return "Service Area";
        case LOCATION_EMERGENCY_LANE: return "Emergency Lane";
        default:                     return "Unknown";
    }
}
