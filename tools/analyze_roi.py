"""
分析实时 ROI 灰度图：查看眼睛是否在 ROI 范围内
"""
import sys
import struct
import numpy as np
from PIL import Image

# 从 stdin 读取 hex 数据（=GRAY_START= 到 =GRAY_END= 之间的行）
def parse_hex_dump(data):
    lines = data.strip().split('\n')
    pixels = []
    for line in lines:
        line = line.strip()
        if not line or line.startswith('==') or line.startswith('W ('):
            continue
        # 每行是连续 hex 字符
        for i in range(0, len(line), 2):
            if i+1 < len(line):
                try:
                    pixels.append(int(line[i:i+2], 16))
                except:
                    pass
    # 可能有换行导致多解析的字节，取前 9216 个
    arr = np.array(pixels, dtype=np.uint8)
    if len(arr) > 96*96:
        arr = arr[:96*96]
    return arr.reshape(96, 96)

# 从文件读取
with open(sys.argv[1], 'r') as f:
    content = f.read()

# 提取 =GRAY_START= 和 =GRAY_END= 之间的内容
start = content.find('==GRAY_START==')
end = content.find('==GRAY_END==')
if start < 0 or end < 0:
    print("ERROR: No GRAY_START/GRAY_END markers found")
    sys.exit(1)

hex_data = content[start+len('==GRAY_START=='):end]
img = parse_hex_dump(hex_data)

print(f"Image shape: {img.shape}")
print(f"Pixel range: {img.min()} - {img.max()}")
print(f"Mean: {img.mean():.1f}, Std: {img.std():.1f}")

# 分析水平投影（每行平均值）— 看垂直方向明暗变化
row_means = img.mean(axis=1)
print("\n=== 垂直投影（行均值 每4行）===")
for r in range(0, 96, 4):
    bar = '█' * int(row_means[r] / 8)
    print(f"  row {r:2d}: {row_means[r]:5.1f} {bar}")

# 分析垂直投影（每列平均值）
col_means = img.mean(axis=0)
print("\n=== 水平投影（列均值 每4列）===")
for c in range(0, 96, 4):
    bar = '█' * int(col_means[c] / 8)
    print(f"  col {c:2d}: {col_means[c]:5.1f} {bar}")

# 找最暗区域（可能是眼睛/眉毛）
dark_thresh = img.mean() - img.std()
dark_mask = img < dark_thresh
dark_rows = np.where(dark_mask.any(axis=1))[0]
dark_cols = np.where(dark_mask.any(axis=0))[0]
if len(dark_rows) > 0:
    print(f"\n最暗区域行范围: {dark_rows[0]} - {dark_rows[-1]} (ROI 垂直位置)")
    print(f"最暗区域列范围: {dark_cols[0]} - {dark_cols[-1]}")

# 找明暗过渡剧烈的行（可能是眼睛 vs 皮肤交界）
row_grad = np.abs(np.diff(row_means))
sharp_rows = np.argsort(row_grad)[-5:][::-1]
print(f"\n垂直变化最剧烈的行: {sorted(sharp_rows)} (可能是眉毛/眼睛位置)")

# 生成灰度图并保存
img_pil = Image.fromarray(img, 'L')
img_pil.save(sys.argv[1] + '.png')
print(f"\n图片已保存: {sys.argv[1] + '.png'}")

# ASCII 艺术预览（缩小版）
print("\n=== ASCII 预览（每2x2取1）===")
chars = ' .:-=+*#%@'
for r in range(0, 96, 3):
    line = ''
    for c in range(0, 96, 3):
        v = img[r, c]
        idx = int(v / 256 * len(chars))
        line += chars[min(idx, len(chars)-1)]
    print(line)
