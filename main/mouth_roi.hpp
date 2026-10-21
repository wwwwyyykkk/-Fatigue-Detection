/**
 * @file mouth_roi.hpp
 * @brief 打哈欠检测 — 暗区比例法 (Dark Pixel Ratio)
 *
 * 原理：打哈欠时口腔内部是黑色空洞，嘴巴 ROI 中心区域暗像素占比飙升。
 *       不需要关键点检测、不需要训练模型，纯图像统计 + 时序过滤。
 *
 * 用法：
 *   1. 从人脸检测框估算嘴巴 ROI → estimate_mouth_roi(face)
 *   2. 裁剪嘴巴区域灰度图 → crop_to_grayN() 或简单采样
 *   3. 计算暗区比例 → compute_dark_ratio(gray, w, h)
 *   4. 时序过滤 → 连续 >=15 帧高暗区比 = 哈欠事件
 */
#pragma once

#include <stdint.h>
#include <algorithm>
#include "dl_detect_define.hpp"
#include "eye_roi.hpp"  // 复用 roi_rect_t, rgb565_frame_t, crop_to_gray96

/* ── 嘴巴 ROI 配置 ──────────────────────────────────────── */
typedef struct {
    float top;      /* Y = face_top + face_h * top     */
    float bottom;   /* Y = face_top + face_h * bottom  */
    float left;     /* X = face_left + face_w * left   */
    float right;    /* X = face_left + face_w * right  */
} mouth_roi_config_t;

/* 默认：嘴巴在人脸框底部区域，底部延伸到框外（打哈欠时下巴下坠）
 * 垂直 55%-115%：从鼻下开始，延伸到下巴下方（捕捉下坠的口腔黑洞）
 * 水平 15%-85%：宽覆盖，捕捉大幅张口的口角 */
static const mouth_roi_config_t kDefaultMouthRoiConfig = {
    .top    = 0.55f,
    .bottom = 1.15f,
    .left   = 0.15f,
    .right  = 0.85f,
};

/* ── 暗区检测参数 ───────────────────────────────────────── */
#define MOUTH_DARK_THRESHOLD    50      /* gray < 50 → "暗像素" (口腔黑洞) */
#define MOUTH_DARK_RATIO_YAWN   0.35f   /* 暗区 > 35% → 张嘴 (打哈欠/说话) */
#define MOUTH_YAWN_DURATION_MS  1500    /* 持续张嘴 1.5 秒 → 哈欠确认 (时间基准，帧率无关) */

/* ── 嘴巴 ROI 估算 ─────────────────────────────────────── */
static inline roi_rect_t estimate_mouth_roi(const dl::detect::result_t &face,
                                             const mouth_roi_config_t *cfg = nullptr)
{
    if (cfg == nullptr) cfg = &kDefaultMouthRoiConfig;

    int fw = face.box[2] - face.box[0];   /* face width  */
    int fh = face.box[3] - face.box[1];   /* face height */

    roi_rect_t roi;
    roi.x = face.box[0] + static_cast<int>(fw * cfg->left);
    roi.y = face.box[1] + static_cast<int>(fh * cfg->top);
    roi.w = static_cast<int>(fw * (cfg->right - cfg->left));
    roi.h = static_cast<int>(fh * (cfg->bottom - cfg->top));

    if (roi.w < 8)  roi.w = 8;
    if (roi.h < 8)  roi.h = 8;
    return roi;
}

/**
 * @brief 计算灰度图中心区域的暗像素占比
 *
 * 只分析中心 60% 区域（去除嘴唇边缘噪点），
 * 统计 gray < MOUTH_DARK_THRESHOLD 的像素占比。
 *
 * @param gray  灰度图数据 (行优先, w × h)
 * @param w     宽度
 * @param h     高度
 * @return float 暗区比例 [0.0, 1.0]
 */
static inline float compute_dark_ratio(const uint8_t *gray, int w, int h)
{
    if (gray == nullptr || w < 4 || h < 4) return 0.0f;

    /* 只分析中心 60% 区域，排除边缘噪点 */
    int cx0 = w  * 20 / 100;   /* 左 20%  */
    int cx1 = w  * 80 / 100;   /* 右 80%  */
    int cy0 = h  * 20 / 100;   /* 上 20%  */
    int cy1 = h  * 80 / 100;   /* 下 80%  */

    int dark_count = 0;
    int total      = 0;

    for (int y = cy0; y < cy1; y++) {
        for (int x = cx0; x < cx1; x++) {
            total++;
            if (gray[y * w + x] < MOUTH_DARK_THRESHOLD) {
                dark_count++;
            }
        }
    }

    if (total == 0) return 0.0f;
    return (float)dark_count / (float)total;
}

/**
 * @brief 从 RGB565 帧裁剪嘴巴 ROI 并双线性缩放到目标尺寸灰度图
 *
 * 复用 crop_to_gray96 的定点数双线性算法，目标尺寸可配置。
 *
 * @param fb       RGB565 帧
 * @param roi      嘴巴 ROI 矩形
 * @param out      输出灰度缓冲 (dst_w × dst_h)
 * @param dst_w    目标宽度
 * @param dst_h    目标高度
 * @return true    成功
 */
static bool crop_mouth_to_gray(const rgb565_frame_t *fb,
                                const roi_rect_t &roi,
                                uint8_t *out,
                                int dst_w, int dst_h)
{
    if (fb == nullptr || fb->buf == nullptr || out == nullptr) return false;

    const int sw = static_cast<int>(fb->width);
    const int sh = static_cast<int>(fb->height);
    const uint8_t *src = fb->buf;

    /* Clamp ROI to image bounds */
    int x0 = std::max(0, std::min(roi.x, sw - 1));
    int y0 = std::max(0, std::min(roi.y, sh - 1));
    int x1 = std::max(0, std::min(roi.x + roi.w, sw));
    int y1 = std::max(0, std::min(roi.y + roi.h, sh));
    int rw = x1 - x0;
    int rh = y1 - y0;
    if (rw < 2 || rh < 2) return false;

    /* Fixed-point bilinear downscale (12-bit fraction) */
    for (int dy = 0; dy < dst_h; dy++) {
        int sy_fixed = (y0 << 12) + (dy << 12) * rh / dst_h + (1 << 11);
        for (int dx = 0; dx < dst_w; dx++) {
            int sx_fixed = (x0 << 12) + (dx << 12) * rw / dst_w + (1 << 11);

            int sx0 = std::max(0, std::min((sx_fixed >> 12) - 1, sw - 2));
            int sy0 = std::max(0, std::min((sy_fixed >> 12) - 1, sh - 2));
            int sx1 = sx0 + 1;
            int sy1 = sy0 + 1;

            int frac_x = (sx_fixed >> 1) & 0x7FF;
            int frac_y = (sy_fixed >> 1) & 0x7FF;
            int w0 = (1 << 11) - frac_x;
            int w1 = frac_x;

            /* RGB565 → grayscale for 4 neighbors */
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

            out[dy * dst_w + dx] = static_cast<uint8_t>(std::max(0, std::min(gray, 255)));
        }
    }
    return true;
}
