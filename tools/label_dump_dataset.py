#!/usr/bin/env python3
"""
从串口 dump 中提取灰度图，显示并让用户标注 open/closed，保存为可用于 Edge Impulse 训练的数据集。

用法：
    1. 运行开发板，把串口输出保存到文件：
       idf.py -p COM16 flash monitor | tee dump.txt
    2. 运行本脚本：
       python label_dump_dataset.py dump.txt --output-dir ./labeled_dataset
    3. 每张图按 'o' (open) / 'c' (closed) / 's' (skip) 标注
    4. 标注完成后，将 labeled_dataset 上传到 Edge Impulse 重新训练
"""

import re
import sys
import argparse
from pathlib import Path
import struct

from PIL import Image
import numpy as np


def extract_grayscale_blocks(text: str):
    """从串口文本中提取所有 ==GRAY_START== ... ==GRAY_END== 块（支持带 label 和不带 label 两种格式）"""
    pattern = r'==GRAY_START(?: label=(\w+))?==\r?\n(.*?)\r?\n==GRAY_END=='
    matches = re.findall(pattern, text, re.DOTALL)
    blocks = []
    for label, data_str in matches:
        label = label or "unknown"
        # 清除 ANSI 转义序列（如 \x1b[33m、\x1b[0m 等）
        data_str = re.sub(r'\x1b\[[0-9;]*[a-zA-Z]', '', data_str)
        # 按行分割，只保留纯十六进制行（滤掉日志单独行）
        lines = data_str.splitlines()
        hex_lines = []
        for line in lines:
            line = line.strip()
            if not line:
                continue
            if line.startswith('W (') or line.startswith('I (') or line.startswith('E ('):
                continue
            line = line.replace('\r', '')
            # 去掉行内嵌的日志（如 "W (61253) Fatigue: Frame buffer underflow"）
            # 其中 F,a,b,e,d 是十六进制字符，会污染数据
            line = re.sub(r'W\s*\(\s*\d+\s*\)\s*Fatigue:.*', '', line)
            hex_lines.append(line)
        # 合并十六进制行
        hex_str = ''.join(hex_lines)
        # 安全清理：只保留 [0-9a-fA-F]
        hex_str = re.sub(r'[^0-9a-fA-F]', '', hex_str)
        if len(hex_str) != 96 * 96 * 2:
            print(f"  [跳过] 长度不对: {len(hex_str)//2} bytes (期望 9216), 实际 {len(hex_str)//2}, label={label}")
            continue
        try:
            data = bytes.fromhex(hex_str)
            blocks.append((label, data))
        except ValueError as e:
            print(f"  [跳过] 十六进制解码失败: {e}, label={label}")
            continue
    return blocks


def main():
    parser = argparse.ArgumentParser(description="从串口 dump 提取眼睛数据集")
    parser.add_argument("dump_file", type=str, help="串口输出文本文件路径")
    parser.add_argument("--output-dir", type=str, default="./labeled_dataset",
                        help="输出目录 (默认 ./labeled_dataset)")
    parser.add_argument("--interactive", action="store_true",
                        help="逐张手动标注（默认按采集时标签自动分类）")
    args = parser.parse_args()

    # 自动检测编码（Tee-Object 默认输出 UTF-16 LE，重定向输出 UTF-8）
    raw = Path(args.dump_file).read_bytes()
    if raw[:2] == b'\xff\xfe':
        text = raw.decode('utf-16-le', errors='replace')
    else:
        text = raw.decode('utf-8', errors='replace')
    blocks = extract_grayscale_blocks(text)
    print(f"找到 {len(blocks)} 组灰度图")

    if len(blocks) == 0:
        print("错误：未找到灰度图数据。请确认串口输出包含 ==GRAY_START== ... ==GRAY_END==")
        sys.exit(1)

    output_dir = Path(args.output_dir)
    open_dir = output_dir / "eye_open"
    closed_dir = output_dir / "eye_closed"
    open_dir.mkdir(parents=True, exist_ok=True)
    closed_dir.mkdir(parents=True, exist_ok=True)

    open_count = 0
    closed_count = 0
    unknown_count = 0

    if not args.interactive:
        # 自动模式：按采集标签分类保存
        print("自动分类模式（采集时已带标签）...")
        for label, data in blocks:
            pixels = np.frombuffer(data, dtype=np.uint8).reshape(96, 96)
            img = Image.fromarray(pixels, 'L')
            if label == "eye_open":
                filename = f"open_{open_count:04d}.jpg"
                img.save(open_dir / filename, quality=95)
                open_count += 1
            elif label == "eye_closed":
                filename = f"closed_{closed_count:04d}.jpg"
                img.save(closed_dir / filename, quality=95)
                closed_count += 1
            else:
                unknown_count += 1
        print("\n完成！")
        print(f"  睁眼(eye_open):   {open_count} 张 → {open_dir}")
        print(f"  闭眼(eye_closed): {closed_count} 张 → {closed_dir}")
        if unknown_count > 0:
            print(f"  跳过(unknown):    {unknown_count} 张（无标签）")
        print(f"\n如需上传 Edge Impulse，将 {output_dir.absolute()} 压缩后上传")
        return

    # 交互模式：逐张手动标注
    print("\n按 'o' 标记睁眼, 'c' 标记闭眼, 's' 跳过, 'q' 退出\n")

    try:
        import tkinter as tk
        from tkinter import ttk
        root = tk.Tk()
        root.withdraw()
    except ImportError:
        pass

    for idx, (label, data) in enumerate(blocks):
        pixels = np.frombuffer(data, dtype=np.uint8).reshape(96, 96)
        img = Image.fromarray(pixels, 'L')

        # 显示图片（终端模式用 ASCII 预览）
        print(f"\n--- 第 {idx+1}/{len(blocks)} 张 (采集标签: {label}) ---")
        # ASCII 预览（缩小到 24x24 字符）
        for y in range(0, 96, 8):
            line = ""
            for x in range(0, 96, 4):
                block = pixels[y:y+8, x:x+4]
                avg = block.mean()
                if avg < 50:
                    line += "██"
                elif avg < 100:
                    line += "▓▓"
                elif avg < 150:
                    line += "▒▒"
                elif avg < 200:
                    line += "░░"
                else:
                    line += "  "
            print(line)

        # 交互输入
        while True:
            try:
                # 尝试用 tkinter 显示图像窗口
                try:
                    import tkinter as tk
                    from PIL import ImageTk
                    win = tk.Toplevel(root)
                    win.title(f"第 {idx+1} 张 - 按 o/c/s/q")
                    photo = ImageTk.PhotoImage(img.resize((384, 384)))
                    label = tk.Label(win, image=photo)
                    label.pack()
                    win.update()

                    def on_key(event):
                        win.quit()
                        win.key_pressed = event.char

                    win.bind('<Key>', on_key)
                    win.focus_set()

                    root.wait_window(win)
                    key = getattr(win, 'key_pressed', 's')
                    win.destroy()
                except:
                    key = input("  [o]睁眼 [c]闭眼 [s]跳过 [q]退出: ").strip().lower()
            except EOFError:
                key = 'q'

            if key == 'o':
                filename = f"open_{open_count:04d}.jpg"
                img.save(open_dir / filename, quality=95)
                open_count += 1
                print(f"  → 保存为 eye_open/{filename}")
                break
            elif key == 'c':
                filename = f"closed_{closed_count:04d}.jpg"
                img.save(closed_dir / filename, quality=95)
                closed_count += 1
                print(f"  → 保存为 eye_closed/{filename}")
                break
            elif key == 's':
                print("  → 跳过")
                break
            elif key == 'q':
                print("\n退出标注")
                break
            else:
                print("  → 无效输入")

        if key == 'q':
            break

    print(f"\n完成！共标注 {open_count} 张睁眼, {closed_count} 张闭眼")
    print(f"数据集保存在: {output_dir.absolute()}")
    print(f"\n如需上传 Edge Impulse，将 {output_dir.absolute()} 压缩后上传")


if __name__ == "__main__":
    main()
