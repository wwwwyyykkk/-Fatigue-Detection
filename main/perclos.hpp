/**
 * perclos.hpp — PERCLOS (Percentage of Eye Closure) 计算器 + 疲劳状态机
 *
 * 使用滑动窗口统计闭眼帧比例，判定疲劳等级：
 *   - ≤15%  → 正常
 *   - 15~30% → 轻度疲劳
 *   - >30%   → 严重疲劳（报警）
 *
 * 线程安全：非可重入，应在同一任务中调用。
 * 窗口大小可模板参数配置，默认 150 帧（30s @ 5fps 处理速率）。
 */

#pragma once
#include <cstdint>
#include <cstring>
#include "esp_log.h"

static const char *PERCLOS_TAG = "PERCLOS";

/* ── 疲劳等级 ── */
enum class FatigueLevel : uint8_t {
    NORMAL   = 0,   // PERCLOS ≤ 15%
    MILD     = 1,   // 15% < PERCLOS ≤ 30%
    SEVERE   = 2,   // PERCLOS > 30%
};

/* ── 默认等级阈值（百分比），天气联动可调 ── */
constexpr float PERCLOS_THRESHOLD_MILD   = 15.0f;
constexpr float PERCLOS_THRESHOLD_SEVERE = 30.0f;

/* ── 天气风险等级 ── */
enum class WeatherRisk : uint8_t {
    CLEAR   = 0,   // 晴/多云，正常阈值
    RAIN    = 1,   // 雨/雪，降 3%
    FOG      = 2,  // 雾/霾/沙尘，降 5%
};

/* ═══════════════════════════════════════════════════════════════
 * 模板化 PERCLOS 计算器（避免动态内存分配）
 * ═══════════════════════════════════════════════════════════════ */
template <int WINDOW_SIZE = 150>
class PerclosCalculator {
public:
    PerclosCalculator()
        : m_head(0), m_count(0), m_closed_count(0) {
        memset(m_buffer, 0, sizeof(m_buffer));
    }

    /* ── 添加一帧的眼睛状态（true=闭眼, false=睁眼） ── */
    void add_sample(bool is_closed) {
        if (m_count < WINDOW_SIZE) {
            /* 窗口未填满 */
            m_buffer[m_head] = is_closed;
            if (is_closed) m_closed_count++;
            m_head = (m_head + 1) % WINDOW_SIZE;
            m_count++;
        } else {
            /* 窗口已满：替换最旧帧 */
            bool old = m_buffer[m_head];
            m_buffer[m_head] = is_closed;
            if (old && !is_closed)       m_closed_count--;
            else if (!old && is_closed)  m_closed_count++;
            m_head = (m_head + 1) % WINDOW_SIZE;
        }
    }

    /* ── 获取当前 PERCLOS 百分比 ── */
    float get_perclos() const {
        if (m_count == 0) return 0.0f;
        return (static_cast<float>(m_closed_count) / m_count) * 100.0f;
    }

    /* ── 设置天气风险，自动调整预警阈值 ── */
    void set_weather_risk(WeatherRisk risk) {
        m_weather_risk = risk;
        switch (risk) {
            case WeatherRisk::CLEAR:
                m_thresh_mild   = 15.0f;
                m_thresh_severe = 30.0f;
                break;
            case WeatherRisk::RAIN:   // 雨/雪 → 更严格
                m_thresh_mild   = 12.0f;
                m_thresh_severe = 25.0f;
                break;
            case WeatherRisk::FOG:    // 雾/霾/沙尘 → 最严格
                m_thresh_mild   = 10.0f;
                m_thresh_severe = 20.0f;
                break;
        }
    }

    /* ── 获取当前天气风险等级 ── */
    WeatherRisk get_weather_risk() const { return m_weather_risk; }

    /* ── 天气联动后的驾驶时长预警阈值（分钟） ── */
    int get_drive_warning_minutes() const {
        switch (m_weather_risk) {
            case WeatherRisk::CLEAR: return 210;   // 3.5h
            case WeatherRisk::RAIN:  return 180;   // 3.0h
            case WeatherRisk::FOG:   return 150;   // 2.5h
            default:                 return 210;
        }
    }

    /* ── 获取疲劳等级（使用天气联动阈值） ── */
    FatigueLevel get_fatigue_level() const {
        float p = get_perclos();
        if (p > m_thresh_severe) return FatigueLevel::SEVERE;
        if (p > m_thresh_mild)   return FatigueLevel::MILD;
        return FatigueLevel::NORMAL;
    }

    /* ── 获取指定 PERCLOS 值的疲劳等级（含哈欠加成等额外因素） ── */
    FatigueLevel get_fatigue_level_for(float perclos) const {
        if (perclos > m_thresh_severe) return FatigueLevel::SEVERE;
        if (perclos > m_thresh_mild)   return FatigueLevel::MILD;
        return FatigueLevel::NORMAL;
    }

    /* ── 获取已采样数（窗口填充进度） ── */
    int get_valid_count() const { return m_count; }

    /* ── 窗口是否已填满（可输出稳定 PERCLOS） ── */
    bool is_warm() const { return m_count >= WINDOW_SIZE; }

    /* ── 获取窗口大小 ── */
    static constexpr int window_size() { return WINDOW_SIZE; }

    /* ── 重置 ── */
    void reset() {
        m_head = 0;
        m_count = 0;
        m_closed_count = 0;
        memset(m_buffer, 0, sizeof(m_buffer));
    }

private:
    bool       m_buffer[WINDOW_SIZE];
    int        m_head;          // 下一个写入位置
    int        m_count;         // 已采样数（<WINDOW_SIZE 时窗口未满）
    int        m_closed_count;  // 窗口中闭眼帧数
    WeatherRisk m_weather_risk = WeatherRisk::CLEAR;
    float      m_thresh_mild   = 15.0f;
    float      m_thresh_severe = 30.0f;
};

/* ═══════════════════════════════════════════════════════════════
 * 疲劳等级转可读字符串
 * ═══════════════════════════════════════════════════════════════ */
inline const char *fatigue_level_str(FatigueLevel lv) {
    switch (lv) {
        case FatigueLevel::NORMAL: return "NORMAL";
        case FatigueLevel::MILD:   return "MILD";
        case FatigueLevel::SEVERE: return "SEVERE";
        default:                   return "UNKNOWN";
    }
}

/* ── 周期日志辅助（每 30 帧或等级变化时输出） ── */
class FatigueLogger {
public:
    /* 疲劳等级变化回调，返回 true=等级已处理 */
    typedef bool (*level_change_cb_t)(FatigueLevel new_level, void *ctx);

    /* 至少采集 30 帧（约 6s）后才输出等级变化告警 */
    static constexpr int WARMUP_SAMPLES = 30;

    FatigueLogger()
        : m_last_lv(FatigueLevel::NORMAL), m_frame_count(0),
          m_change_cb(NULL), m_change_ctx(NULL) {}

    void set_level_change_cb(level_change_cb_t cb, void *ctx) {
        m_change_cb = cb;
        m_change_ctx = ctx;
    }

    void update(FatigueLevel lv, float perclos, int valid_count) {
        m_frame_count++;
        bool changed = (lv != m_last_lv);

        if (changed || (m_frame_count % 30 == 0)) {
            ESP_LOGI(PERCLOS_TAG, "PERCLOS=%.1f%% [%d/%d] %s%s",
                     perclos, valid_count,
                     PerclosCalculator<>::window_size(),
                     fatigue_level_str(lv),
                     changed ? " ★" : "");
            if (changed && valid_count >= WARMUP_SAMPLES) {
                if (lv == FatigueLevel::SEVERE) {
                    ESP_LOGW(PERCLOS_TAG, ">>> 严重疲劳警告！<<<");
                } else if (lv == FatigueLevel::MILD && m_last_lv == FatigueLevel::NORMAL) {
                    ESP_LOGW(PERCLOS_TAG, ">>> 轻度疲劳，建议休息 <<<");
                }
                /* Invoke level-change callback (e.g., for audio playback) */
                if (m_change_cb) {
                    m_change_cb(lv, m_change_ctx);
                }
            }
            m_last_lv = lv;
        }
    }

private:
    FatigueLevel    m_last_lv;
    int             m_frame_count;
    level_change_cb_t m_change_cb;
    void           *m_change_ctx;
};
