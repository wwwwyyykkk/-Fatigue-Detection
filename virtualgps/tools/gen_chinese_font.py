"""
生成 16x16 中文点阵字库 C 头文件
用于 SSD1306 OLED (128x64) 显示

使用策略：先渲染高清灰度图，NEAREST 缩放保留硬边缘，
然后用较高阈值二值化，确保笔画清晰不糊
"""
from PIL import Image, ImageFont, ImageDraw

# 需要的中文字 (按 GB2312 编码排序)
CHARS = "速度位置司机时间普通路段服务区应急车道公里南北东西经纬驾驶长"

def char_to_gb2312(ch):
    """获取 GB2312 编码"""
    raw = ch.encode('gb2312')
    return (raw[0] << 8) | raw[1]

def render_char_16x16(ch):
    """
    将中文字渲染为 16x16 1-bit 点阵
    返回 32 字节 (16行 × 16bit)
    """
    font_path = "C:/Windows/Fonts/simhei.ttf"

    # 大尺寸渲染（60px）→ NEAREST 缩放到 16x16 保留清晰边缘
    big_size = 64
    font = ImageFont.truetype(font_path, 48)
    # 用灰度模式渲染
    big = Image.new('L', (big_size, big_size), 255)
    draw = ImageDraw.Draw(big)
    draw.text((8, 8), ch, font=font, fill=0)

    # NEAREST 缩放保留硬边缘
    small = big.resize((16, 16), Image.NEAREST)

    pixels = small.load()
    data = bytearray(32)
    for row in range(16):
        byte_val = 0
        for col in range(16):
            if pixels[col, row] < 196:  # 阈值 196，保留清晰笔画
                byte_val |= (1 << (15 - col))
        data[row * 2] = (byte_val >> 8) & 0xFF
        data[row * 2 + 1] = byte_val & 0xFF
    return bytes(data)

def main():
    lines = []
    lines.append("/*")
    lines.append(" * chinese_font.h — 16x16 中文点阵字库 (自动生成)")
    lines.append(" * 共 %d 个汉字，每个 32 字节" % len(CHARS))
    lines.append(" */")
    lines.append("#pragma once")
    lines.append("#include <stdint.h>")
    lines.append("")

    # 生成每个字的点阵
    for ch in CHARS:
        try:
            bitmap = render_char_16x16(ch)
            gb = char_to_gb2312(ch)
            var_name = "f_0x%04X" % gb
            lines.append("/* [%s] GB2312=0x%04X */" % (ch, gb))
            lines.append("static const uint8_t %s[32] = {" % var_name)
            for i in range(4):
                row_bytes = bitmap[i*8:(i+1)*8]
                hex_str = ", ".join("0x%02X" % b for b in row_bytes)
                lines.append("    %s," % hex_str)
            lines.append("};")
            lines.append("")
        except Exception as e:
            print("Error rendering '%s': %s" % (ch, e))
            gb = char_to_gb2312(ch)
            var_name = "f_0x%04X" % gb
            lines.append("static const uint8_t %s[32] = {0};" % var_name)
            lines.append("")

    # 字符索引表 (按 GB2312 排序)
    sorted_chars = sorted(CHARS, key=lambda c: char_to_gb2312(c))
    lines.append("/* 字符查找表 (按 GB2312 编码排序) */")
    lines.append("typedef struct { uint16_t gb; const uint8_t *data; } chinese_char_t;")
    lines.append("")
    lines.append("static const chinese_char_t chinese_font_table[] = {")
    for ch in sorted_chars:
        gb = char_to_gb2312(ch)
        var_name = "f_0x%04X" % gb
        lines.append("    { 0x%04X, %s }," % (gb, var_name))
    lines.append("};")
    lines.append("#define CHINESE_FONT_COUNT %d" % len(sorted_chars))
    lines.append("")

    # 内联查找函数
    lines.append("static inline const uint8_t* chinese_font_lookup(uint16_t gb_code) {")
    lines.append("    int lo = 0, hi = CHINESE_FONT_COUNT - 1;")
    lines.append("    while (lo <= hi) {")
    lines.append("        int mid = (lo + hi) / 2;")
    lines.append("        if (chinese_font_table[mid].gb == gb_code)")
    lines.append("            return chinese_font_table[mid].data;")
    lines.append("        if (gb_code < chinese_font_table[mid].gb)")
    lines.append("            hi = mid - 1;")
    lines.append("        else")
    lines.append("            lo = mid + 1;")
    lines.append("    }")
    lines.append("    return NULL;")
    lines.append("}")

    with open("chinese_font.h", "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print("Generated chinese_font.h with %d characters" % len(CHARS))

    # Also generate a preview
    with open("font_preview.txt", "w", encoding="utf-8") as f:
        for ch in CHARS:
            gb = char_to_gb2312(ch)
            var_name = "f_0x%04X" % gb
            bitmap = render_char_16x16(ch)
            f.write("[%s] GB2312=0x%04X\n" % (ch, gb))
            for row in range(16):
                bits = (bitmap[row*2] << 8) | bitmap[row*2+1]
                line = ""
                for col in range(16):
                    line += "██" if (bits & (1 << (15 - col))) else "  "
                f.write(line + "\n")
            f.write("\n")
    print("Preview saved to font_preview.txt")

if __name__ == "__main__":
    main()
