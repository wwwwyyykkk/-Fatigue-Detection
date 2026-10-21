/**
 * syn6288.h - SYN6288 TTS 语音合成驱动 (软件UART, 不占硬件UART)
 *
 * 引脚:
 *   TX   -> GPIO13 (软件模拟 UART, bit-bang)
 *   BUSY -> GPIO7  (忙检测, 高=正在播)
 *
 * UART1 (GPIO15/16) 完全空闲, 可分配给 GPS 模块
 * 协议: 9600bps 8N1, 帧头 0xFD, GBK 编码, XOR 校验
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 预设语音 ID */
typedef enum {
    SYN_VOICE_SYS_BOOT,         /* 系统初始化完成 */
    SYN_VOICE_OVERSPEED,        /* 检测到超速，请减速行驶 */
    SYN_VOICE_SERVICE_AREA,     /* 当前位置为服务区，可以安全停车 */
    SYN_VOICE_EMERGENCY_LANE,   /* 警告，应急车道禁止停车 */
    SYN_VOICE_DRIVE_3H,         /* 已连续驾驶三小时 */
    SYN_VOICE_DRIVE_3_5H,       /* 已连续驾驶三小时三十分钟... */
    SYN_VOICE_DRIVE_4H,         /* 已连续驾驶四小时，请立即停车休息 */
    SYN_VOICE_REST_DONE,        /* 休息时间已满，可以继续出发 */
    SYN_VOICE_DRIVER_SWITCH,    /* 司机身份已切换，请重新进行人脸验证 */
    SYN_VOICE_MILD_FATIGUE,     /* 检测到轻度疲劳，请注意休息 */
    SYN_VOICE_SEVERE_FATIGUE,   /* 严重疲劳警告，请立即停车休息 */
    SYN_VOICE_ALCOHOL_WARNING,  /* 检测到酒精，请勿驾驶 */
    SYN_VOICE_DRIVER1_OK,       /* 司机1人脸验证成功 */
    SYN_VOICE_DRIVER2_OK,       /* 司机2人脸验证成功 */
    SYN_VOICE_IDENTIFY_START,   /* 开始进行身份验证 */
    SYN_VOICE_IDENTIFY_FAIL,    /* 验证失败请重试 */
    SYN_VOICE_REST_START,       /* 已进入休息模式 */
    SYN_VOICE_NEED_STOP,        /* 请先停车再休息 */
    SYN_VOICE_NEED_SERVICE,     /* 请到服务区再休息 */
    SYN_VOICE_REST_EXIT,        /* 已退出休息，可以继续驾驶 */
    SYN_VOICE_NOT_CLEAR,        /* 没听清，请再说一次 */
    SYN_VOICE_VOLUME_UP,        /* 音量已调高 */
    SYN_VOICE_VOLUME_DOWN,      /* 音量已调低 */
    SYN_VOICE_I_AM_HERE,        /* 我在(唤醒回应) */
    SYN_VOICE_COUNT
} syn_voice_id_t;

/**
 * @brief 设置音量 (0-15, 默认 1)
 * @param vol 音量 0..15, 越界自动钳位
 */
void syn6288_set_volume(int vol);

/**
 * @brief 获取当前音量
 * @return 0..15
 */
int syn6288_get_volume(void);

/**
 * @brief 初始化硬件UART + SYN6288
 * @return true 成功
 */
bool syn6288_init(void);

/**
 * @brief 播放预设语音（非阻塞，发完即返回）
 * @param id  预设语音 ID
 * @return true 已发送
 */
bool syn6288_speak(syn_voice_id_t id);

/**
 * @brief 动态合成预测语音: "预计%d分钟后进入轻度/严重疲劳"
 * @param minutes 预测分钟数 (>=0)
 * @param level   预测目标等级 1=轻度 2=严重
 * @return true 已发送
 */
bool syn6288_speak_prediction(int minutes, int level);

/**
 * @brief 检查是否正在播放
 * @return true 正在播（BUSY=高）
 */
bool syn6288_is_busy(void);

/**
 * @brief 等待当前播放完成
 */
void syn6288_wait_done(void);

/**
 * @brief 运行完整硬件诊断（GPIO检查 + 测试帧 + 硬件清单）
 */
void syn6288_run_diagnostics(void);

/**
 * @brief 扫描常见波特率，找到 SYN6288 实际使用的波特率
 *       在每个波特率下发送测试帧并监控 BUSY 信号
 */
void syn6288_scan_baud_rate(void);

#ifdef __cplusplus
}
#endif
