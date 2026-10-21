/**
 * app_priv.h — VirtualGPS RainMaker 私有定义
 *
 * 定义 GPS 状态 / 疲劳状态 / 远程控制三个 RainMaker 设备
 * 以及上报函数和回调接口
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <esp_rmaker_core.h>
#include <esp_rmaker_standard_types.h>
#include <esp_rmaker_standard_params.h>
#include <esp_rmaker_standard_devices.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 全局 RainMaker 设备指针 ─────────────────────────── */
extern esp_rmaker_device_t *gps_device;
extern esp_rmaker_device_t *fatigue_device;
extern esp_rmaker_device_t *control_device;

/* ── RainMaker 初始化 ────────────────────────────────── */
esp_err_t rainmaker_init(void);

/* ── 事件处理 (WiFi 配网 / MQTT / OTA) ──────────────── */
void rainmaker_register_event_handlers(void);

/* ── 数据上报接口 ───────────────────────────────────── */

/**
 * @brief 上报 GPS 状态到 RainMaker 云端
 * @param speed_kmh      当前速度 (km/h)
 * @param location       地点类型 (0=普通 1=服务区 2=应急)
 * @param driver_id      司机ID (1/2)
 * @param drive_seconds  累计驾驶秒数
 * @param lat            纬度
 * @param lng            经度
 * @param use_real_gps   true=真实GPS false=模拟
 */
void rainmaker_report_gps_state(float speed_kmh, int location,
                                int driver_id, int drive_seconds,
                                float lat, float lng, bool use_real_gps);

/**
 * @brief 上报疲劳状态到 RainMaker 云端
 * @param fatigue_level    疲劳等级 (0=正常 1=轻度 2=严重)
 * @param face_verified    人脸已识别 (0/1)
 * @param alcohol          酒精检测 (0/1)
 * @param driver_confirmed 已确认司机ID (0=未知)
 * @param sys_status       系统状态 (1=正常 2=摄像头断开)
 */
void rainmaker_report_fatigue_state(int fatigue_level, int face_verified,
                                    int alcohol, int driver_confirmed,
                                    int sys_status);

/**
 * @brief 启动周期性 GPS 状态上报任务
 *        每 5 秒上报一次当前 GPS 状态到云端
 */
void rainmaker_start_periodic_report(void);

#ifdef __cplusplus
}
#endif
