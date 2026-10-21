"""
正确解析 UTF-16LE dump 文件的灰度图
关键：不要按行分割 hex，整段取前 18432 个 hex 字符
"""
import re, sys
import numpy as np
from PIL import Image
from pathlib import Path

dump_file = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('dump_live.txt')
out_dir = Path('dump5_parsed')
out_dir.mkdir(exist_ok=True)

raw = dump_file.read_bytes()

# 探测编码
if raw[:2] == b'\xff\xfe':
    text = raw.decode('utf-16-le')
    print("Encoding: UTF-16 LE")
else:
    text = raw.decode('utf-8', errors='replace')
    print("Encoding: UTF-8")

# 找到所有 GRAY_START .. GRAY_END 段
count = 0
search_start = 0
while True:
    start = text.find('==GRAY_START', search_start)
    if start < 0:
        break
    # 跳过本行（GRAY_START 标记行后的第一个换行开始就是数据）
    nl1 = text.index('\n', start)
    data_begin = nl1 + 1

    end = text.find('==GRAY_END==', data_begin)
    if end < 0:
        break

    # 提取标签
    label_m = re.search(r'label=(\w+)', text[start:nl1])
    label = label_m.group(1) if label_m else 'unknown'

    # 提取数据块
    block = text[data_begin:end]

    # 清理
    block = re.sub(r'\x1b\[[0-9;]*[a-zA-Z]', '', block)  # ANSI 转义
    # 去掉整行日志：以 W(数字) / I(数字) / E(数字) 开头（整行删除）
    block = re.sub(r'^[WIWE]\s*\(\s*\d+\s*\).*$', '', block, flags=re.MULTILINE)
    # 去掉残留的行内 W(数字)Fatigue:xxx 段
    block = re.sub(r'[WIWE]\s*\(\s*\d+\s*\)\s*Fatigue:\s*[a-zA-Z_ ]+', '', block)

    # 关键：只保留纯 hex 字符，无视换行
    hex_str = re.sub(r'[^0-9a-fA-F]', '', block)

    print(f"\n--- Image {count} (label={label}) ---")
    print(f"  Raw hex chars after cleanup: {len(hex_str)}")

    if len(hex_str) < 18432:  # 9216 像素 × 2 hex 字符每像素
        print(f"  ERROR - Too few: {len(hex_str)} (need >= 18432)")
        search_start = end + 1
        continue

    # 取前 9216 个像素的 hex
    hex_str = hex_str[:18432]
    # 转二进制
    data = bytes.fromhex(hex_str)
    # 成形
    img_array = np.frombuffer(data, dtype=np.uint8).reshape(96, 96)
    img = Image.fromarray(img_array, 'L')

    # 保存
    label_dir = out_dir / (label if label != 'unknown' else 'unknown')
    label_dir.mkdir(exist_ok=True)
    fname = f'{label}_{count:04d}.png'
    img.save(label_dir / fname)

    print(f"  OK - Saved {fname}")
    print(f"  Pixel range: {img_array.min()} - {img_array.max()}")
    print(f"  Mean: {img_array.mean():.1f}, Std: {img_array.std():.1f}")

    # ASCII 预览
    chars = ' .:-=+*#%@'
    print(f"  Preview (vertical):")
    for r in range(0, 96, 6):
        line = ''
        for c in range(0, 96, 3):
            v = img_array[r, c]
            idx = int(v / 256 * len(chars))
            line += chars[min(idx, len(chars)-1)]
        print(f"    {line}")

    count += 1
    search_start = end + 1

print(f"\n共提取 {count} 张图片 → {out_dir}/")
