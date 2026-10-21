"""
generate_simulated_perclos_csv.py
生成模拟 PERCLOS 数据集 CSV，供显示屏开发人员离线调试 PERCLOS 曲线绘制。

数据特征：
  - 前 30 帧：正常驾驶（偶有眨眼）→ PERCLOS ≈ 5~10%
  - 31~90 帧：逐渐疲劳（眨眼增多、闭眼变长）→ PERCLOS 从 10% 爬升到 40%
  - 91~120 帧：严重疲劳（频繁闭眼）→ PERCLOS ≈ 40~60%
  - 121~150 帧：靠边休息恢复 → PERCLOS 回落到 10% 以下

输出：D:\Fatigue-detection\output\simulated_perclos.csv
"""

import csv
import math
import random

random.seed(42)
OUTPUT = r"D:\Fatigue-detection\output\simulated_perclos.csv"

NUM_FRAMES = 200
WINDOW_SIZE = 30          # 当前固件使用 PerclosCalculator<30>

# ── 场景设计 ──
def get_blink_pattern(frame_idx):
    """返回这一帧的 eye_open, eye_closed 概率（模拟不同阶段）"""
    # 基础：正常状态 open=0.95 closed=0.05
    base_open = 0.95
    base_closed = 0.05

    if frame_idx < 30:
        # 阶段A：正常驾驶，偶有短暂眨眼
        # 每 5~8 帧随机眨一次眼（1~2 帧长度）
        if frame_idx % 7 in (0, 1) and random.random() < 0.5:
            return 0.10, 0.90
        return 0.95, 0.05

    elif frame_idx < 90:
        # 阶段B：疲劳积累，眨眼更频繁、闭眼更长
        # 闭眼事件概率上升
        if frame_idx % 5 == 0 and random.random() < 0.6:
            return 0.05, 0.95
        if frame_idx % 11 == 0:
            return 0.30, 0.70
        # 加入随机闭眼串（连续 2~3 帧闭眼）
        if frame_idx % 13 == 0 or frame_idx % 17 == 0:
            return 0.08, 0.92
        # 半闭状态（眼睑下垂）
        if frame_idx % 19 == 0:
            return 0.55, 0.45
        return 0.88, 0.12

    elif frame_idx <= 120:
        # 阶段C：严重疲劳，频繁长时间闭眼
        if frame_idx % 3 == 0 and random.random() < 0.7:
            return 0.02, 0.98
        if frame_idx % 8 == 0:
            return 0.01, 0.99
        if frame_idx % 13 == 0:
            return 0.04, 0.96
        # 睁眼也像半闭
        return 0.70, 0.30

    else:
        # 阶段D：休息后恢复，逐渐回到正常
        rest_progress = (frame_idx - 120) / 50.0  # 0~1
        if rest_progress < 0.3:
            # 刚休息，还有些疲劳
            if frame_idx % 6 == 0:
                return 0.10, 0.90
            return 0.80, 0.20
        else:
            # 恢复良好
            if frame_idx % 8 == 0 and random.random() < 0.4:
                return 0.10, 0.90
            return 0.94, 0.06


# ── 模拟滑动窗口 PERCLOS ──
buffer = [False] * WINDOW_SIZE
head = 0
count = 0
closed_count = 0

rows = []
for i in range(NUM_FRAMES):
    eye_open, eye_closed = get_blink_pattern(i)
    is_closed = eye_closed > eye_open

    # 更新滑动窗口
    if count < WINDOW_SIZE:
        buffer[head] = is_closed
        if is_closed:
            closed_count += 1
        head = (head + 1) % WINDOW_SIZE
        count += 1
    else:
        old = buffer[head]
        buffer[head] = is_closed
        if old and not is_closed:
            closed_count -= 1
        elif not old and is_closed:
            closed_count += 1
        head = (head + 1) % WINDOW_SIZE

    perclos = (closed_count / count) * 100.0 if count > 0 else 0.0

    # 疲劳等级
    if perclos > 30.0:
        level = "SEVERE"
    elif perclos > 15.0:
        level = "MILD"
    else:
        level = "NORMAL"

    # 时间戳（假设 5fps 处理速率）
    timestamp_ms = i * 200  # 每帧 200ms

    rows.append({
        "frame": i,
        "timestamp_ms": timestamp_ms,
        "eye_open": round(eye_open, 4),
        "eye_closed": round(eye_closed, 4),
        "is_closed": int(is_closed),
        "perclos_pct": round(perclos, 2),
        "fatigue_level": level,
        "window_count": count,
    })

# ── 写入 CSV ──
with open(OUTPUT, "w", newline="") as f:
    writer = csv.DictWriter(f, fieldnames=[
        "frame", "timestamp_ms", "eye_open", "eye_closed",
        "is_closed", "perclos_pct", "fatigue_level", "window_count",
    ])
    writer.writeheader()
    writer.writerows(rows)

print(f"✅ 已生成 {OUTPUT}")
print(f"   共 {len(rows)} 帧数据")
print(f"   PERCLOS 范围: {min(r['perclos_pct'] for r in rows):.1f}% ~ {max(r['perclos_pct'] for r in rows):.1f}%")
print()

# ── 打印前 40 行快速预览 ──
print("=" * 100)
print(f"{'frame':>6} | {'time(ms)':>10} | {'eye_open':>9} | {'eye_closed':>10} | {'closed':>6} | {'PERCLOS%':>8} | {'level':>8} | {'wsize':>6}")
print("-" * 100)
for r in rows[:40]:
    print(f"{r['frame']:>6} | {r['timestamp_ms']:>10} | {r['eye_open']:>9.4f} | {r['eye_closed']:>10.4f} | "
          f"{r['is_closed']:>6} | {r['perclos_pct']:>8.2f} | {r['fatigue_level']:>8} | {r['window_count']:>6}")
print("... (后略)")
print(f"\n各阶段数据分布:")
for phase_name, start, end in [("Phase A 正常驾驶", 0, 30), ("Phase B 疲劳积累", 30, 90),
                                  ("Phase C 严重疲劳", 90, 120), ("Phase D 休息恢复", 120, 200)]:
    phase_rows = [r for r in rows if start <= r["frame"] < end]
    avg_p = sum(r["perclos_pct"] for r in phase_rows) / len(phase_rows)
    print(f"  {phase_name:<20} frames {start:>3}~{end:>3}  avg PERCLOS={avg_p:>6.2f}%")
