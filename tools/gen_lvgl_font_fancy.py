#!/usr/bin/env python3
"""Generate LVGL 9.5.x HOLLOW (outline) script clock font.
- Digits stretched taller (render big, resize to 32x56)
- Outline extraction: only the border pixels remain (hollow/neon look)
- 2px outline thickening
"""
from PIL import Image, ImageFont, ImageDraw

FONT_PATH = "C:/Windows/Fonts/simhei.ttf"   # 黑体 (normal bold)
RENDER_SIZE = 50        # render large, then resize down for tall+narrow digits
CHARS = "0123456789:.-"
OUT = "D:/Fatigue-detection/Project/main/lv_font_fancy_44.c"
NAME = "lv_font_fancy_44"
BOX_W_DIGIT = 32        # multiple of 8
BOX_W_NARROW = 16
BOX_H = 56
LINE_H = 56
THICKEN = 3             # outline thickness (thicker)

font = ImageFont.truetype(FONT_PATH, RENDER_SIZE)

def make_glyph(c, wbox):
    img0 = Image.new("L", (300, 300), 0)
    d0 = ImageDraw.Draw(img0)
    d0.text((4, 4), c, fill=255, font=font)
    bbox = img0.getbbox()
    if not bbox:
        return None
    glyph = img0.crop(bbox)
    # Resize to target box (tall + narrow)
    glyph = glyph.resize((wbox, BOX_H), Image.LANCZOS)
    return glyph

def to_outline(img):
    """Keep only edge pixels (hollow)."""
    W, H = img.size
    out = img.copy()
    px = img.load()
    ox = out.load()
    for y in range(H):
        for x in range(W):
            if px[x, y] > 128:
                has_empty = False
                for nx, ny in ((x-1,y),(x+1,y),(x,y-1),(x,y+1)):
                    if 0 <= nx < W and 0 <= ny < H and px[nx, ny] <= 128:
                        has_empty = True
                        break
                if not has_empty:
                    ox[x, y] = 0
    return out

def thicken(img, n):
    for _ in range(n):
        src = img.copy()
        px_src = src.load()
        px = img.load()
        W, H = img.size
        for y in range(H):
            for x in range(W):
                if px_src[x, y] > 128:
                    if x + 1 < W: px[x+1, y] = 255
                    if y + 1 < H: px[x, y+1] = 255
    return img

glyphs = []
for c in CHARS:
    wbox = BOX_W_NARROW if c in ":." else BOX_W_DIGIT
    g = make_glyph(c, wbox)
    if g is None:
        continue
    # g = to_outline(g)        # hollow (removed — use solid fill)
    g = thicken(g, THICKEN)    # bold strokes
    row_bytes = wbox // 8
    bitmap = bytearray(BOX_H * row_bytes)
    for row in range(BOX_H):
        for byte_i in range(row_bytes):
            bval = 0
            for bit in range(8):
                col = byte_i * 8 + bit
                if g.getpixel((col, row)) > 128:
                    bval |= (0x80 >> bit)
            bitmap[row * row_bytes + byte_i] = bval
    glyphs.append((c, ord(c), wbox, bytes(bitmap)))

glyphs.sort(key=lambda g: g[1])

out = []
out.append(f"/* {NAME}.c — HOLLOW script clock font (thicken {THICKEN}) */")
out.append('#include "lvgl.h"')
out.append("")

out.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
bitmap_offset = 0
for c, unicode, wbox, bm in glyphs:
    out.append(f"    {{.bitmap_index = {bitmap_offset}, .adv_w = {wbox*16}, "
               f".box_w = {wbox}, .box_h = {BOX_H}, .ofs_x = 0, .ofs_y = 0}},")
    bitmap_offset += len(bm)
out.append("};")
out.append("")

out.append("static const uint8_t glyph_bitmap[] = {")
for c, unicode, wbox, bm in glyphs:
    hex_str = ",".join(f"0x{b:02X}" for b in bm)
    out.append(f"    // U+{unicode:04X} '{c}'")
    out.append(f"    {hex_str},")
out.append("};")
out.append("")

min_u = glyphs[0][1]
out.append(f"static const uint16_t unicode_list[] = {{")
for c, unicode, wbox, bm in glyphs:
    out.append(f"    0x{unicode - min_u:04X}, // '{c}' U+{unicode:04X}")
out.append("};")
out.append("")

max_u = glyphs[-1][1]
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

out.append(f"const lv_font_t {NAME} = {{")
out.append("    .dsc = &font_dsc,")
out.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
out.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
out.append(f"    .line_height = {LINE_H},")
out.append("    .base_line = 0,")
out.append("    .subpx = LV_FONT_SUBPX_NONE,")
out.append("    .underline_position = -2,")
out.append("    .underline_thickness = 1,")
out.append("};")

content = "\n".join(out)
with open(OUT, "w", encoding="utf-8") as f:
    f.write(content)

clock_w = 6 * BOX_W_DIGIT + 2 * BOX_W_NARROW
print(f"Generated {len(glyphs)} glyphs, {bitmap_offset} bytes, box {BOX_W_DIGIT}x{BOX_H}")
print(f"Clock 'HH:MM:SS' width: {clock_w}px (screen 240)")
