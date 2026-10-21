#!/usr/bin/env python3
"""Generate LVGL 9.5.x compatible 16px Chinese bitmap font C file.
Usage: gen_lvgl_font.py [font_path] [output_path] [font_name] [size]
"""
import sys
from PIL import Image, ImageFont, ImageDraw

FONT_ARG = sys.argv[1] if len(sys.argv) > 1 else "C:/Windows/Fonts/simsun.ttc"
OUT_ARG  = sys.argv[2] if len(sys.argv) > 2 else "D:/Fatigue-detection/Project/main/lv_font_chinese_16.c"
NAME_ARG = sys.argv[3] if len(sys.argv) > 3 else "lv_font_chinese_16"
SIZE_ARG = int(sys.argv[4]) if len(sys.argv) > 4 else 16
THICKEN  = int(sys.argv[5]) if len(sys.argv) > 5 else 0

# 全量字符集 = 现有 lv_font_chinese_16.c 全部 179 字符 + 新字 哈欠加（哈欠/加成，2026-08-05）
# 注意：必须是全量，缺字会导致 UI 中已有汉字变方框。
CHARS = (
    " !#$%&'()*+,-./0123456789:;<=>?@ABCDEFGH"
    "IJKLMNOPQRSTUVWXYZ[]^_abcdefghijklmnopqr"
    "stuvwxyz{}一三严中二五人令份休位停像公六出分别前功加务劳区司周命哈四在"
    "失头对小已常年应度开待态急息成接提摄无日时普月有服机欠正段测状疲监秒等纬经置脸行"
    "证识试请败路身车轻进连退通速道里重钟长驶驾验"
)
ASCII = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!@#$%^&*()-_=+[]{};:',.<>/? "

seen = set()
chars = []
for c in CHARS + ASCII:
    if c not in seen:
        seen.add(c)
        chars.append(c)

font = ImageFont.truetype(FONT_ARG, SIZE_ARG)
glyphs = []
for c in chars:
    img = Image.new("L", (16, 16), 0)
    draw = ImageDraw.Draw(img)
    bbox = font.getbbox(c)
    if bbox:
        tw = bbox[2] - bbox[0]
        th = bbox[3] - bbox[1]
        x = (16 - tw) // 2 - bbox[0]
        y = (16 - th) // 2 - bbox[1]
    else:
        x = y = 0
    draw.text((x, y), c, fill=255, font=font)
    # Optional stroke thickening: expand each lit pixel to the right by 1px
    if THICKEN > 0:
        for _ in range(THICKEN):
            for row in range(16):
                for col in range(15, 0, -1):
                    if img.getpixel((col - 1, row)) > 128:
                        img.putpixel((col, row), 255)
    bitmap = bytearray(32)
    for row in range(16):
        b0 = 0
        for col in range(8):
            if img.getpixel((col, row)) > 128:
                b0 |= (0x80 >> col)
        bitmap[row * 2] = b0
        b1 = 0
        for col in range(8, 16):
            if img.getpixel((col, row)) > 128:
                b1 |= (0x80 >> (col - 8))
        bitmap[row * 2 + 1] = b1
    glyphs.append((c, ord(c), bytes(bitmap)))

glyphs.sort(key=lambda g: g[1])

out = []
out.append(f"/* {NAME_ARG}.c — Auto-generated LVGL 9.5.x {SIZE_ARG}px CJK font */")
out.append('#include "lvgl.h"')
out.append("")

# Glyph descriptors (LVGL 9.5.x format: bitmap_index:20, adv_w:12, box_w, box_h, ofs_x, ofs_y)
# adv_w is in 4.4 format: 16 * 16 = 256
out.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
bitmap_offset = 0
for c, unicode, _ in glyphs:
    out.append(f"    {{.bitmap_index = {bitmap_offset}, .adv_w = {16*16}, "
               f".box_w = 16, .box_h = 16, .ofs_x = 0, .ofs_y = 0}},")
    bitmap_offset += 32
out.append("};")
out.append("")

# Bitmap data
out.append("static const uint8_t glyph_bitmap[] = {")
for c, unicode, bitmap in glyphs:
    hex_bytes = ",".join(f"0x{b:02X}" for b in bitmap)
    out.append(f"    // U+{unicode:04X} '{c}'")
    out.append(f"    {hex_bytes},")
out.append("};")
out.append("")

# CMAP (sparse tiny format) — needs relative codepoints
min_u = glyphs[0][1]
max_u = glyphs[-1][1]

# Unicode list (SPARSE_TINY format: relative codepoints = unicode - range_start)
out.append(f"// SPARSE_TINY: relative codepoints (rcp = unicode - 0x{min_u:04X})")
out.append(f"static const uint16_t unicode_list[] = {{")
for c, unicode, _ in glyphs:
    rcp = unicode - min_u
    out.append(f"    0x{rcp:04X}, // '{c}' U+{unicode:04X}")
out.append("};")
out.append("")
out.append("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
out.append("    {")
out.append(f"        .range_start = 0x{min_u:04X},")
out.append(f"        .range_length = 0x{max_u - min_u + 1:X},")
out.append("        .glyph_id_start = 0,")
out.append("        .unicode_list = unicode_list,")
out.append("        .glyph_id_ofs_list = NULL,")
out.append(f"        .list_length = {len(glyphs)},")
out.append("        .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY,")
out.append("    }")
out.append("};")
out.append("")

# Font descriptor (LVGL 9.5.x)
out.append("static const lv_font_fmt_txt_dsc_t font_dsc = {")
out.append("    .glyph_bitmap = glyph_bitmap,")
out.append("    .glyph_dsc = glyph_dsc,")
out.append("    .cmaps = cmaps,")
out.append("    .kern_dsc = NULL,")
out.append("    .kern_scale = 0,")
out.append("    .cmap_num = 1,")
out.append("    .bpp = 1,")
out.append("    .kern_classes = 0,")
out.append("    .bitmap_format = LV_FONT_FMT_TXT_PLAIN,")
out.append("};")
out.append("")

# Font object
out.append(f"const lv_font_t {NAME_ARG} = {{")
out.append("    .dsc = &font_dsc,")
out.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
out.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
out.append("    .line_height = 16,")
out.append("    .base_line = 0,")
out.append("    .subpx = LV_FONT_SUBPX_NONE,")
out.append("    .underline_position = -2,")
out.append("    .underline_thickness = 1,")
out.append("};")

content = "\n".join(out)
with open(OUT_ARG, "w", encoding="utf-8") as f:
    f.write(content)

print(f"Generated {len(glyphs)} glyphs ({bitmap_offset} bytes)")
