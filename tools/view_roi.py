"""
更严格的 ROI 灰度图解析器 — 过滤掉任何非纯 hex 的行
"""
import sys
import numpy as np
from PIL import Image

def is_pure_hex_line(line):
    """检查一行是否全部是纯 hex 字符（忽略空格）"""
    s = line.strip()
    if not s or len(s) < 20:  # 太短的跳过
        return False
    # 纯 hex 行应该只有 0-9 a-f 和空格
    for ch in s:
        if ch not in '0123456789abcdef ':
            return False
    return True

with open(sys.argv[1]) as f:
    lines = f.readlines()

# 提取纯 hex 行
hex_parts = []
capture = False
for line in lines:
    stripped = line.strip()

    # 检测 GRAY_START 标记
    if 'GRAY_START' in stripped:
        capture = True
        continue
    if 'GRAY_END' in stripped:
        capture = False
        break

    # 在 ==GRAY_START== 和 ==GRAY_END== 之间的行
    if capture:
        # 只保留纯 hex 行，跳过所有日志行
        if is_pure_hex_line(stripped):
            hex_parts.append(stripped)
            print(f"  KEPT: {stripped[:40]}...")
        else:
            print(f"  SKIP: {stripped[:60]}")

if not hex_parts:
    print("ERROR: No pure hex lines found!")
    sys.exit(1)

# 合并所有 hex
all_hex = ''.join(hex_parts).replace(' ', '')
print(f"\nTotal hex chars: {len(all_hex)}")

pixels = []
had_error = False
for i in range(0, min(len(all_hex), 96*96*2), 2):
    if i+1 < len(all_hex):
        try:
            pixels.append(int(all_hex[i:i+2], 16))
        except:
            had_error = True
            break

if len(pixels) > 96*96:
    pixels = pixels[:96*96]
elif len(pixels) < 96*96:
    last = pixels[-1] if pixels else 128
    pixels += [last] * (96*96 - len(pixels))
    print(f"Padded to {96*96} pixels")

img = np.array(pixels, dtype=np.uint8).reshape(96, 96)
print(f"\nFinal image: {img.shape}")
print(f"Pixel range: {img.min()} - {img.max()}")
print(f"Mean: {img.mean():.1f}, Std: {img.std():.1f}")
print(f"Min value at: {np.unravel_index(img.argmin(), img.shape)} (row, col)")
print(f"Max value at: {np.unravel_index(img.argmax(), img.shape)} (row, col)")

# 保存大图
scale = 8
out_path = sys.argv[1].rsplit('.', 1)[0] + '.png'
Image.fromarray(img, 'L').resize((96*scale, 96*scale), Image.NEAREST).save(out_path, quality=95)
print(f"\nSaved: {out_path} ({96*scale}x{96*scale})")
