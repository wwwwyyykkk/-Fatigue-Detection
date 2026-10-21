"""
从串口日志中提取 base64 灰度图并显示/保存
用法:
   1. 把 === 96x96 GRAY DUMP (base64) === 到 === END GRAY DUMP ===
      之间的日志保存到文件，运行:
     python decode_eye_dump.py < log.txt
"""

import sys, base64, re
from PIL import Image
import io

def extract_base64(text):
    """从日志文本中提取 base64 数据"""
    lines = text.split('\n')
    in_dump = False
    b64_chunks = []

    for line in lines:
        # 去掉 ESP-IDF 日志前缀 (I (xxxx) TAG: ...)
        clean = re.sub(r'^[IWEF] \(\d+\) \w+:?\s*', '', line).strip()

        if '=== 96x96 GRAY DUMP (base64) ===' in clean:
            in_dump = True
            continue
        if '=== END GRAY DUMP ===' in clean:
            break
        if not in_dump:
            continue
        if not clean:
            continue
        # 跳过非 base64 行（警告等）
        if re.search(r'[^A-Za-z0-9+/=]', clean):
            continue
        b64_chunks.append(clean)

    return ''.join(b64_chunks)


def decode_gray96(b64_str):
    """解码 base64 到 96x96 灰度图"""
    raw = base64.b64decode(b64_str)
    # 期望 9216 字节
    if len(raw) < 96 * 96:
        print(f"WARN: 只有 {len(raw)} 字节，预期 {96*96}")
        # 补零
        raw = raw + b'\x00' * (96*96 - len(raw))
    elif len(raw) > 96 * 96:
        raw = raw[:96*96]

    img = Image.frombytes('L', (96, 96), raw)
    return img


if __name__ == '__main__':
    text = sys.stdin.read()
    b64 = extract_base64(text)

    if not b64:
        print("错误：未找到 base64 dump 数据！")
        print("请确保从串口日志中复制了包含 === 96x96 GRAY DUMP ===")
        print("到 === END GRAY DUMP === 的完整内容。")
        sys.exit(1)

    print(f"提取到 {len(b64)} 个 base64 字符")

    img = decode_gray96(b64)

    # 显示基本信息
    print(f"图片尺寸: {img.size[0]}x{img.size[1]}")
    pixels = list(img.getdata())
    print(f"像素范围: {min(pixels)} - {max(pixels)}")
    print(f"平均亮度: {sum(pixels)/len(pixels):.1f}/255")

    # 保存
    img.save('eye_dump_debug.png')
    print("已保存 eye_dump_debug.png")

    # 放大 4 倍方便查看
    img_big = img.resize((384, 384), Image.NEAREST)
    img_big.save('eye_dump_4x.png')
    print("已保存 eye_dump_4x.png (4倍放大)")

    # 显示直方图信息
    dark = sum(1 for p in pixels if p < 50)
    mid = sum(1 for p in pixels if 50 <= p < 150)
    bright = sum(1 for p in pixels if p >= 150)
    print(f"暗部(<50): {dark} ({100*dark/9216:.1f}%)")
    print(f"中部(50-150): {mid} ({100*mid/9216:.1f}%)")
    print(f"亮部(>150): {bright} ({100*bright/9216:.1f}%)")
