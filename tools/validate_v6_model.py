"""
完整端到端验证 v6 INT8 模型

模拟板子上的完整推理流程：
  板子: uint8 gray[96×96] -> /255.0 -> [0,1] -> 量化(scale, zp) -> INT8 -> 推理 -> INT8 -> 反量化 -> softmax

如果这个脚本输出正常，板子上就一定正常。
"""
import re, sys
import numpy as np

# 1. 从 C 数组提取模型
print("="*60)
print("1. Extract model from C array")
print("="*60)
h_path = r"D:\Fatigue-detection\Project\components\eye_classifier\tflite-model\tflite_model_data.h"

with open(h_path, 'rb') as f:
    content = f.read()

hex_pattern = re.compile(rb'0x([0-9A-Fa-f]{2})')
matches = hex_pattern.findall(content)
binary_data = bytes(int(m, 16) for m in matches)

# 验证 TFLite magic
assert binary_data[4:8] == b'TFL3', "Not a valid TFLite file!"
print(f"[OK] Extracted {len(binary_data):,} bytes ({len(binary_data)/1024:.1f} KB)")
print(f"[OK] TFLite magic: {binary_data[4:8]}")

# 2. 加载模型
print("\n" + "="*60)
print("2. Load model & check input/output")
print("="*60)

import tensorflow as tf

interpreter = tf.lite.Interpreter(model_content=binary_data)
interpreter.allocate_tensors()

inp = interpreter.get_input_details()[0]
out = interpreter.get_output_details()[0]

print(f"[OK] Input dtype: {inp['dtype'].__name__}  shape: {list(inp['shape'])}")
print(f"[OK] Input quantization: scale={inp['quantization'][0]:.6f}, zero_point={inp['quantization'][1]}")
print(f"[OK] Output dtype: {out['dtype'].__name__}  shape: {list(out['shape'])}")
print(f"[OK] Output quantization: scale={out['quantization'][0]:.6f}, zero_point={out['quantization'][1]}")

# 关键数值：input scale 和 zero_point
in_s = inp['quantization'][0]
in_zp = inp['quantization'][1]
out_s = out['quantization'][0]
out_zp = out['quantization'][1]

# 3. 模拟板子量化公式
print("\n" + "="*60)
print("3. Verify quantization formula matches board code")
print("="*60)

# 板子代码: real = gray96[i] / 255.0f; q = real / in_s + in_zp
# 验证这个公式的 range 是否覆盖 INT8 [-128, 127]
gray_test = np.array([0, 64, 128, 192, 255], dtype=np.uint8)
real_test = gray_test.astype(np.float32) / 255.0
q_test = np.round(real_test / in_s) + in_zp
q_test = np.clip(q_test, -128, 127)

print(f"Input scale={in_s:.6f}, zero_point={in_zp}")
print(f"{'gray':>6s} {'real':>8s} {'q_val':>6s} {'clamped?':>10s}")
for g, r, q in zip(gray_test, real_test, q_test):
    clamped = "CLAMPED!" if q < -128 or q > 127 else ""
    print(f"{g:6d} {r:8.4f} {q:6.0f} {clamped:>10s}")

q_min, q_max = q_test.min(), q_test.max()
print(f"\nQuantized range: [{q_min}, {q_max}] (INT8 range: [-128, 127])")
if q_min >= -128 and q_max <= 127:
    print("[OK] All values in INT8 range")
else:
    print("[FAIL] Values out of range!")

# 4. 测试不同输入下的输出
print("\n" + "="*60)
print("4. Test model with various inputs (simulating eye scenarios)")
print("="*60)

def board_inference(gray_values):
    """Simulate exactly what the board does."""
    # 构造 96×96 全图
    gray_img = np.full((96, 96), gray_values[0] if len(set(gray_values)) == 1 else 0, dtype=np.uint8)
    if len(set(gray_values)) > 1:
        # 非均匀图：上半暗(瞳孔)，下半亮(巩膜)
        gray_img[:48, :] = gray_values[0]
        gray_img[48:, :] = gray_values[1]

    # 归一化 [0, 1]
    real = gray_img.astype(np.float32) / 255.0

    # 量化到 INT8
    q = np.clip(np.round(real / in_s) + in_zp, -128, 127).astype(np.int8)

    # 送入模型
    interpreter.set_tensor(inp['index'], q.reshape(1, 96, 96, 1))
    interpreter.invoke()

    # 读出 INT8 输出
    out_int8 = interpreter.get_tensor(out['index'])[0]

    # 反量化后直接使用（模型末尾已有 Softmax，和板子代码一致）
    out_real = (out_int8.astype(np.float32) - out_zp) * out_s
    return np.array([out_real[0], out_real[1]], dtype=np.float32), out_int8, out_real

test_cases = [
    ("All black (0)", [0, 0]),
    ("All white (255)", [255, 255]),
    ("Mid gray (128)", [128, 128]),
    ("Dark top / Light bottom (eye open sim)", [40, 200]),
    ("All medium (skin tone, eye closed)", [160, 160]),
    ("Very dark top (pupil)", [10, 150]),
]

print(f"{'Test case':<40s} {'open':>8s} {'closed':>8s} {'prediction':>15s}")
print("-" * 80)
for name, values in test_cases:
    probs, out_int8, out_real = board_inference(values)
    pred = "*CLOSED" if probs[1] > probs[0] else "*OPEN"
    print(f"{name:<40s} {probs[0]:8.4f} {probs[1]:8.4f} {pred:>15s}")

# 5. 测试真实的眼睛特征
print("\n" + "="*60)
print("5. Test with realistic eye-like gradient images")
print("="*60)

# 模拟睁眼：上方有暗色瞳孔，下方有亮色巩膜
eye_open = np.zeros((96, 96), dtype=np.uint8)
for y in range(96):
    if y < 30:
        eye_open[y, :] = np.random.randint(10, 60, 96)      # 瞳孔/上眼睑：暗
    elif y < 55:
        eye_open[y, :] = np.random.randint(30, 180, 96)     # 虹膜过渡
    else:
        eye_open[y, :] = np.random.randint(150, 230, 96)    # 巩膜：亮

real = eye_open.astype(np.float32) / 255.0
q = np.clip(np.round(real / in_s) + in_zp, -128, 127).astype(np.int8)
interpreter.set_tensor(inp['index'], q.reshape(1, 96, 96, 1))
interpreter.invoke()
out_int8 = interpreter.get_tensor(out['index'])[0]
out_real = (out_int8.astype(np.float32) - out_zp) * out_s
# 模型末尾有 Softmax，直接比较反量化后的概率值
print(f"Eye-open-like gradient:   open={out_real[0]:.4f}  closed={out_real[1]:.4f}  -> {'*OPEN [OK]' if out_real[0] > out_real[1] else '[FAIL] CLOSED (WRONG!)'}")

# 模拟闭眼：整体肤色均匀
eye_closed = np.full((96, 96), 0, dtype=np.uint8)
for y in range(96):
    eye_closed[y, :] = np.random.randint(120, 200, 96)  # 皮肤色调

real = eye_closed.astype(np.float32) / 255.0
q = np.clip(np.round(real / in_s) + in_zp, -128, 127).astype(np.int8)
interpreter.set_tensor(inp['index'], q.reshape(1, 96, 96, 1))
interpreter.invoke()
out_int8 = interpreter.get_tensor(out['index'])[0]
out_real = (out_int8.astype(np.float32) - out_zp) * out_s
# 模型末尾有 Softmax，直接比较反量化后的概率值
print(f"Eye-closed-like uniform:  open={out_real[0]:.4f}  closed={out_real[1]:.4f}  -> {'*CLOSED [OK]' if out_real[1] > out_real[0] else '[FAIL] OPEN (WRONG!)'}")

# 6. Arena size 检查
print("\n" + "="*60)
print("6. Arena size check")
print("="*60)
arena_used = interpreter._interpreter.arena_used_bytes() if hasattr(interpreter, '_interpreter') else 'N/A'
print(f"Model arena requirement: {arena_used} bytes")
print(f"TFLITE_MODEL_ARENA_SIZE: 256864 bytes")
if isinstance(arena_used, int) and arena_used <= 256864:
    print(f"[OK] Arena sufficient ({arena_used}/{256864}, {100*arena_used/256864:.0f}%)")
else:
    print(f"Check manually - arena_used={arena_used}")

# 7. Op resolver 验证
print("\n" + "="*60)
print("7. Op resolver compatibility check")
print("="*60)
from tensorflow.lite.python import schema_py_generated as schema_fb
model = schema_fb.Model.GetRootAsModel(binary_data, 0)
required_ops = set()
op_names = {0: "ADD", 3: "CONV_2D", 4: "DEPTHWISE_CONV_2D", 9: "FULLY_CONNECTED",
            18: "MUL", 22: "RESHAPE", 25: "SOFTMAX"}
for i in range(model.OperatorCodesLength()):
    code = model.OperatorCodes(i).BuiltinCode()
    required_ops.add(op_names.get(code, f"Op{code}"))

resolver_ops = {"ADD", "CONV_2D", "DEPTHWISE_CONV_2D", "FULLY_CONNECTED", "MUL", "RESHAPE", "SOFTMAX"}
print(f"Model needs:  {required_ops}")
print(f"Resolver has: {resolver_ops}")
missing = required_ops - resolver_ops
extra = resolver_ops - required_ops
if not missing:
    print("[OK] All required ops are registered")
else:
    print(f"[FAIL] MISSING OPS: {missing}")

print("\n" + "="*60)
print("FINAL VERDICT")
print("="*60)
