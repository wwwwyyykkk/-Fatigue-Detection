"""
Generate 16x16 Chinese bitmap font for TFT status panel
"""
from PIL import Image, ImageFont, ImageDraw

# All needed Chinese characters for TFT status panel
CHARS = "".join(dict.fromkeys("状态正常轻度严重未知脸无效有分相似司机驾驶长速度位置时间普通路段服务区紧急车道公里南北东西经纬"))

def char_to_gb2312(ch):
    raw = ch.encode('gb2312')
    return (raw[0] << 8) | raw[1]

def render_char_16x16(ch):
    font_path = "C:/Windows/Fonts/simhei.ttf"
    big_size = 64
    font = ImageFont.truetype(font_path, 48)
    big = Image.new('L', (big_size, big_size), 0)  # black bg
    draw = ImageDraw.Draw(big)
    draw.text((8, 8), ch, font=font, fill=255)  # white text
    small = big.resize((16, 16), Image.NEAREST)
    pixels = small.load()
    data = bytearray(32)
    for row in range(16):
        hi = 0
        lo = 0
        for col in range(16):
            if pixels[col, row] > 128:
                if col < 8:
                    hi |= (1 << (7 - col))
                else:
                    lo |= (1 << (15 - col))
        data[row * 2] = hi
        data[row * 2 + 1] = lo
    return bytes(data)

def main():
    lines = []
    lines.append("/* tft_chinese.h — 16x16 Chinese font for TFT (auto-generated) */")
    lines.append("#pragma once")
    lines.append("#include <stdint.h>")
    lines.append("")

    for ch in CHARS:
        bitmap = render_char_16x16(ch)
        gb = char_to_gb2312(ch)
        var_name = "g_0x%04X" % gb
        lines.append("/* %s GB=0x%04X */" % (ch, gb))
        lines.append("static const uint8_t %s[32] = {" % var_name)
        for i in range(4):
            row_bytes = bitmap[i*8:(i+1)*8]
            lines.append("    " + ", ".join("0x%02X" % b for b in row_bytes) + ",")
        lines.append("};")
        lines.append("")

    sorted_chars = sorted(CHARS, key=lambda c: char_to_gb2312(c))
    lines.append("typedef struct { uint16_t gb; const uint8_t *data; } gb_char_t;")
    lines.append("static const gb_char_t gb_font_table[] = {")
    for ch in sorted_chars:
        gb = char_to_gb2312(ch)
        lines.append("    { 0x%04X, g_0x%04X }," % (gb, gb))
    lines.append("};")
    lines.append("#define GB_FONT_COUNT %d" % len(sorted_chars))
    lines.append("")

    lines.append("static inline const uint8_t* gb_font_lookup(uint16_t code) {")
    lines.append("    int lo = 0, hi = GB_FONT_COUNT - 1;")
    lines.append("    while (lo <= hi) {")
    lines.append("        int mid = (lo + hi) / 2;")
    lines.append("        if (gb_font_table[mid].gb == code) return gb_font_table[mid].data;")
    lines.append("        if (code < gb_font_table[mid].gb) hi = mid - 1;")
    lines.append("        else lo = mid + 1;")
    lines.append("    }")
    lines.append("    return NULL;")
    lines.append("}")

    with open("tft_chinese.h", "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("Generated tft_chinese.h with %d chars" % len(CHARS))

if __name__ == "__main__":
    main()
