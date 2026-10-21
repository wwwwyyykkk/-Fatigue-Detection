#!/usr/bin/env python3
"""解析串口 dump 中的灰度图数据并可视化（无需 numpy）"""
import re, sys
from pathlib import Path
from PIL import Image

if len(sys.argv) < 2:
    print("用法: python parse_dump.py <dump_file.txt>")
    sys.exit(1)

text = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")

# 提取 GRAY_START...GRAY_END
pattern = r'==GRAY_START==\n(.*?)\n==GRAY_END=='
matches = re.findall(pattern, text, re.DOTALL)
print(f"找到 {len(matches)} 组灰度图\n")

for idx, block in enumerate(matches):
    hex_str = block.replace('\n', '').replace(' ', '').replace('\r', '')
    if len(hex_str) != 96 * 96 * 2:
        print(f"[{idx}] 跳过: 长度={len(hex_str)//2} bytes (需要 9216)")
        continue
    raw = bytes.fromhex(hex_str)
    pixels = list(raw)

    lo = min(pixels)
    hi = max(pixels)
    avg = sum(pixels) // len(pixels)

    print(f"=== 第 {idx+1} 组 ===")
    print(f"  像素范围: [{lo}, {hi}]")
    print(f"  均值: {avg}")

    # ASCII 可视化（缩小到 24x48）
    for y in range(0, 96, 4):
        line = ""
        for x in range(0, 96, 2):
            block_avg = sum(pixels[y*96 + x:(y+4)*96 + x+2]) / 8
            if block_avg < 40:    line += "█"
            elif block_avg < 60:  line += "▓"
            elif block_avg < 80:  line += "▒"
            elif block_avg < 100: line += "░"
            elif block_avg < 120: line += "·"
            elif block_avg < 140: line += "."
            elif block_avg < 160: line += "-"
            elif block_avg < 180: line += ","
            else:                 line += " "
        print(line)

    # 保存为 PNG
    img = Image.new('L', (96, 96))
    img.putdata(pixels)
    out = Path(f"eye_dump_{idx}.png")
    img.save(out)
    print(f"  -> 已保存 {out}")
    print()
