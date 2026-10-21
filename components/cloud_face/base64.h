/**
 * base64.h — 纯头文件 Base64 编码器
 *
 * 用法：
 *   int len = base64_encode(src, src_len, dst, dst_size);
 *   dst[len] = '\0';
 *
 * 输出不包含换行符。dst_size 需 >= ((src_len + 2) / 3) * 4 + 1。
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

static const char _b64_tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline int base64_encode(const uint8_t *src, size_t len, char *out, size_t out_max)
{
    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t a = (i < len) ? src[i++] : 0;
        uint32_t b = (i < len) ? src[i++] : 0;
        uint32_t c = (i < len) ? src[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        if (j + 4 > out_max) return -1;
        out[j++] = _b64_tab[(triple >> 18) & 0x3F];
        out[j++] = _b64_tab[(triple >> 12) & 0x3F];
        out[j++] = _b64_tab[(triple >>  6) & 0x3F];
        out[j++] = _b64_tab[ triple        & 0x3F];
    }
    /* fix padding */
    size_t mod = len % 3;
    if (mod == 1) { out[j - 2] = '='; out[j - 1] = '='; }
    else if (mod == 2) { out[j - 1] = '='; }
    return (int)j;
}
