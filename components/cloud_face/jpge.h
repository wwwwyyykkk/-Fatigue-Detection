/**
 * jpge.h — 微型 JPEG 编码器 (public domain)
 * 仅支持 baseline JPEG, 8-bit RGB→YCoCg→DCT→Huffman
 *
 * 用法:
 *   int jpg_len = jpge_encode_rgb888(buf, w, h, 3, out, out_max, 75);
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 将 RGB888 像素编码为 JPEG
 * @param pixels   RGB888 像素数据 (按行排列)
 * @param w        宽度
 * @param h        高度
 * @param channels 3(RGB) 或 1(gray)
 * @param out      输出缓冲
 * @param out_size 输出缓冲大小
 * @param quality  1-100 (推荐 70)
 * @return JPEG 数据长度, <=0 表示失败
 */
int jpge_encode_rgb888(const uint8_t *pixels, int w, int h, int channels,
                        uint8_t *out, int out_size, int quality);

#ifdef __cplusplus
}
#endif
