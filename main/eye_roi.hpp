/**
 * @file eye_roi.hpp
 * @brief 人脸框 → 眼睛ROI 估算 + 裁剪缩放灰度管道
 *
 * 从 ESP-DL 人脸检测结果中提取人脸框，
 * 根据人脸比例估算眼睛区域，裁剪并缩放至 96×96 灰度图。
 */

#pragma once

#include <stdint.h>
#include <algorithm>
#include <cmath>

#include "dl_detect_define.hpp"

/**
 * @brief 简易 RGB565 帧描述（替代 camera_fb_t，解除对 esp32-camera 的依赖）
 */
struct rgb565_frame_t {
    uint8_t *buf;      /* 像素数据起始地址 */
    int      width;    /* 图像宽度（像素）  */
    int      height;   /* 图像高度（像素）  */
};

/* ── 眼睛ROI 配置参数 ───────────────────────────────────── */
typedef struct {
    /* 眼睛区域在人脸框中的垂直范围 (比例) */
    float eye_roi_top;      /* 起始 Y = face_top + face_h * eye_roi_top    */
    float eye_roi_bottom;   /* 结束 Y = face_top + face_h * eye_roi_bottom */
    /* 水平范围 (比例) */
    float eye_roi_left;     /* 起始 X = face_left + face_w * eye_roi_left  */
    float eye_roi_right;    /* 结束 X = face_left + face_w * eye_roi_right */
} eye_roi_config_t;

/* 默认参数
 * 垂直范围 28%~58%（30% 高度）：保留足够像素避免放大过度模糊
 * 水平范围 05%~95%
 *
 * v4 恢复宽 ROI：之前追求"紧贴训练数据"把 ROI 缩到 10%-17% 高度，
 * 导致图像放大 5-6 倍完全模糊，模型无法提取特征。
 * 神经网络对包含少量额外上下文（眉毛/额头）有鲁棒性，
 * 但对极端模糊没有。
 */
static const eye_roi_config_t kDefaultEyeRoiConfig = {
    .eye_roi_top    = 0.22f,
    .eye_roi_bottom = 0.48f,
    .eye_roi_left   = 0.05f,
    .eye_roi_right  = 0.60f,
};

/* ── 矩形定义 ───────────────────────────────────────────── */
typedef struct {
    int x, y;       /* 左上角 */
    int w, h;       /* 宽高 */
} roi_rect_t;

/**
 * @brief 从人脸检测结果估算眼睛 ROI
 *
 * @param face  ESP-DL 人脸检测结果 (box = [left, top, right, bottom])
 * @param cfg   配置参数 (传 NULL 用默认值)
 * @return roi_rect_t  眼睛区域 (已 clamp 到图像边界)
 *
 * @note 对于侧面脸或大角度人脸，估算可能不准，
 *       后续可用人脸关键点 (landmark) 提高精度
 */
static inline roi_rect_t estimate_eye_roi(const dl::detect::result_t &face,
                                           const eye_roi_config_t *cfg = nullptr)
{
    if (cfg == nullptr) cfg = &kDefaultEyeRoiConfig;

    int fw = face.box[2] - face.box[0];   /* face width  */
    int fh = face.box[3] - face.box[1];   /* face height */

    roi_rect_t roi;
    roi.x = face.box[0] + static_cast<int>(fw * cfg->eye_roi_left);
    roi.y = face.box[1] + static_cast<int>(fh * cfg->eye_roi_top);
    roi.w = static_cast<int>(fw * (cfg->eye_roi_right - cfg->eye_roi_left));
    roi.h = static_cast<int>(fh * (cfg->eye_roi_bottom - cfg->eye_roi_top));

    /* 确保最小尺寸 */
    if (roi.w < 8)  roi.w = 8;
    if (roi.h < 8)  roi.h = 8;
    return roi;
}

/**
 * @brief 从 RGB565 帧中裁剪 ROI，双线性缩放至 96×96 灰度图
 *
 * @param fb     摄像头帧 (必须为 RGB565 格式)
 * @param roi    裁剪矩形
 * @param out    输出缓冲 (96×96 字节)
 * @return true  成功
 * @return false 失败 (参数无效)
 */
static bool crop_to_gray96(const rgb565_frame_t *fb, const roi_rect_t &roi, uint8_t out[96 * 96])
{
    if (fb == nullptr || fb->buf == nullptr) {
        return false;
    }

    const int sw = static_cast<int>(fb->width);
    const int sh = static_cast<int>(fb->height);
    const uint8_t *src = fb->buf;

    /* 将 ROI clamp 到图像范围内 */
    int x0 = std::max(0, std::min(roi.x, sw - 1));
    int y0 = std::max(0, std::min(roi.y, sh - 1));
    int x1 = std::max(0, std::min(roi.x + roi.w, sw));
    int y1 = std::max(0, std::min(roi.y + roi.h, sh));
    int rw = x1 - x0;
    int rh = y1 - y0;
    if (rw < 1 || rh < 1) return false;

    constexpr int DST_W = 96;
    constexpr int DST_H = 96;

    /* 用定点数计算避免浮点开销：
     *   将比例放大到 12 位小数 (4096)
     */

    for (int dy = 0; dy < DST_H; dy++) {
        int sy_fixed = (y0 << 12) + (dy << 12) * rh / DST_H + (1 << 11);

        for (int dx = 0; dx < DST_W; dx++) {
            int sx_fixed = (x0 << 12) + (dx << 12) * rw / DST_W + (1 << 11);

            int sx0 = std::max(0, std::min((sx_fixed >> 12) - 1, sw - 2));
            int sy0 = std::max(0, std::min((sy_fixed >> 12) - 1, sh - 2));
            int sx1 = sx0 + 1;
            int sy1 = sy0 + 1;

            int frac_x = (sx_fixed >> 1) & 0x7FF;
            int frac_y = (sy_fixed >> 1) & 0x7FF;
            int w0 = (1 << 11) - frac_x;
            int w1 = frac_x;

            /* ── 读取 4 个邻近像素的 RGB565 → 灰度 ── */
            auto rgb565_to_gray = [&](int px, int py) -> int {
                int idx = (py * sw + px) * 2;
                uint16_t p = (static_cast<uint16_t>(src[idx]) << 8) | src[idx + 1];
                int r = (p >> 11) & 0x1F;  r = (r << 3) | (r >> 2);
                int g = (p >>  5) & 0x3F;  g = (g << 2) | (g >> 4);
                int b =  p        & 0x1F;  b = (b << 3) | (b >> 2);
                return (int)(0.299f * r + 0.587f * g + 0.114f * b);
            };

            int g00 = rgb565_to_gray(sx0, sy0);
            int g10 = rgb565_to_gray(sx1, sy0);
            int g01 = rgb565_to_gray(sx0, sy1);
            int g11 = rgb565_to_gray(sx1, sy1);

            int top = g00 * w0 + g10 * w1;
            int bot = g01 * w0 + g11 * w1;
            int gray = (top * ((1 << 11) - frac_y) + bot * frac_y) >> 22;

            out[dy * DST_W + dx] = static_cast<uint8_t>(std::max(0, std::min(gray, 255)));
        }
    }

    /* 直接返回双线性缩放后的灰度图，不做对比度拉伸。
     * 之前加的 5% 自适应拉伸会把亮暗帧推向纯黑/纯白二值图，
     * 破坏模型需要的连续灰阶信息。模型在合理光照下可以正常工作。 */
    return true;
}
