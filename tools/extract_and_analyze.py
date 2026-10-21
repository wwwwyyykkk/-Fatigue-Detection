"""
Extract TFLite model from C array and analyze expected input normalization.

The device actually uses tflite_model_data.h (9.9MB C array, claims "v5 float32"),
NOT tflite_learn_1020434_3.tflite (216KB INT8 v4).

This script:
1. Extracts the binary model from the C array
2. Tests different input normalizations to determine what the model expects
"""
import sys
import os
import re
import numpy as np

# Parse the C array into binary
def parse_c_array(header_path):
    """Read a C header file containing a byte array and extract binary data."""
    with open(header_path, 'rb') as f:
        content = f.read()

    # Find the array definition
    # Look for pattern: "0xNN, 0xNN, ..."
    hex_pattern = re.compile(rb'0x([0-9A-Fa-f]{2})')
    matches = hex_pattern.findall(content)

    data = bytearray()
    for m in matches:
        data.append(int(m, 16))

    return bytes(data)

# Main
header_path = r"D:\Fatigue-detection\Project\components\eye_classifier\tflite-model\tflite_model_data.h"
output_tflite = r"D:\Fatigue-detection\tools\extracted_v5_model.tflite"

print(f"Parsing C array from: {header_path}")
binary_data = parse_c_array(header_path)
print(f"Extracted {len(binary_data)} bytes ({len(binary_data)/1024:.1f} KB)")

# Check TFLite magic
if len(binary_data) >= 8:
    magic = binary_data[4:8]
    print(f"TFLite magic: {magic} (expected b'TFL3')")
    if magic != b'TFL3':
        print("WARNING: Not a valid TFLite file!")
        # Try to find TFL3 magic in the data
        idx = binary_data.find(b'TFL3')
        if idx >= 0:
            print(f"Found TFL3 at offset {idx}")
            binary_data = binary_data[idx-4:]  # include the version prefix
            print(f"Trimmed to {len(binary_data)} bytes starting from version field")

# Save extracted model
with open(output_tflite, 'wb') as f:
    f.write(binary_data)
print(f"Saved extracted model to: {output_tflite}")

# Now analyze with TFLite
try:
    import tensorflow as tf
except ImportError:
    print("\nTensorFlow not available for this Python. Trying manual analysis...")
    print("Please run the model through tensorflow manually.")
    sys.exit(0)

print("\n" + "="*60)
print("LOADING EXTRACTED MODEL")
print("="*60)

interpreter = tf.lite.Interpreter(model_path=output_tflite)
interpreter.allocate_tensors()

input_details = interpreter.get_input_details()
output_details = interpreter.get_output_details()

print("\n=== INPUT ===")
for d in input_details:
    print(f"  name: {d['name']}")
    print(f"  shape: {d['shape']}")
    print(f"  dtype: {d['dtype']}")
    q = d.get('quantization')
    print(f"  quantization: {q}")
    is_int8 = (d['dtype'] == np.int8)

print("\n=== OUTPUT ===")
for d in output_details:
    print(f"  name: {d['name']}")
    print(f"  shape: {d['shape']}")
    print(f"  dtype: {d['dtype']}")
    print(f"  quantization: {d.get('quantization')}")

print("\n=== TENSOR DETAILS (first 15) ===")
tensor_details = interpreter.get_tensor_details()
for t in tensor_details[:15]:
    print(f"  idx={t['index']:3d} name={t['name']:<40s} shape={str(t['shape']):<20s} dtype={t['dtype']}")

print("\n" + "="*60)
if is_int8:
    print("MODEL IS INT8 - Testing quantization and input range")
else:
    print("MODEL IS FLOAT32 - Testing input normalization range")
print("="*60)

if is_int8:
    # For INT8, test different quantization interpretations
    scale = input_details[0]['quantization'][0]
    zp = input_details[0]['quantization'][1]
    print(f"\nInput quantization: scale={scale}, zero_point={zp}")
    print(f"This means: real_value = (int8_value - {zp}) * {scale}")
    print(f"Expected input range: [{-128 * scale:.4f}, {127 * scale:.4f}] = [{(0-zp)*scale:.4f}, {(255-zp)*scale:.4f}]")

    # Test with various raw pixel values, quantizing them properly
    print("\n=== Testing with raw pixel values quantized to INT8 ===")
    for raw_val in [0, 32, 64, 96, 128, 160, 192, 224, 255]:
        real_val = raw_val / 255.0  # [0, 1]
        q_val = int(round(real_val / scale)) + zp
        q_val = max(-128, min(127, q_val))
        test_input = np.full((1, 96, 96, 1), q_val, dtype=np.int8)
        interpreter.set_tensor(input_details[0]["index"], test_input)
        interpreter.invoke()
        output = interpreter.get_tensor(output_details[0]["index"])

        out_scale = output_details[0]['quantization'][0]
        out_zp = output_details[0]['quantization'][1]
        deq_logits = (output[0].astype(np.float32) - out_zp) * out_scale
        softmax = tf.nn.softmax(deq_logits).numpy()
        print(f"  Raw {raw_val:3d} -> q={q_val:4d}: deq_logits={deq_logits}  softmax={softmax}")

else:
    # For FLOAT32, test what input range the model expects
    print("\n=== Test 1: Input range [0, 1] (pixel/255) ===")
    for raw_val in [0, 32, 64, 96, 128, 160, 192, 224, 255]:
        normalized = raw_val / 255.0
        test_input = np.full((1, 96, 96, 1), normalized, dtype=np.float32)
        interpreter.set_tensor(input_details[0]["index"], test_input)
        interpreter.invoke()
        output = interpreter.get_tensor(output_details[0]["index"])
        softmax = tf.nn.softmax(output[0]).numpy()
        print(f"  gray={raw_val:3d} norm={normalized:.4f}: logits={output[0]}  softmax={softmax}")

    print("\n=== Test 2: Input range [-1, 1] (pixel-127.5)/127.5 ===")
    for raw_val in [0, 32, 64, 96, 128, 160, 192, 224, 255]:
        normalized = (raw_val - 127.5) / 127.5
        test_input = np.full((1, 96, 96, 1), normalized, dtype=np.float32)
        interpreter.set_tensor(input_details[0]["index"], test_input)
        interpreter.invoke()
        output = interpreter.get_tensor(output_details[0]["index"])
        softmax = tf.nn.softmax(output[0]).numpy()
        print(f"  gray={raw_val:3d} norm={normalized:+.4f}: logits={output[0]}  softmax={softmax}")

    print("\n=== Test 3: Input range [0, 255] (raw uint8 as float) ===")
    for raw_val in [0, 32, 64, 96, 128, 160, 192, 224, 255]:
        test_input = np.full((1, 96, 96, 1), float(raw_val), dtype=np.float32)
        interpreter.set_tensor(input_details[0]["index"], test_input)
        interpreter.invoke()
        output = interpreter.get_tensor(output_details[0]["index"])
        softmax = tf.nn.softmax(output[0]).numpy()
        print(f"  raw_float={raw_val:3d}: logits={output[0]}  softmax={softmax}")

    # Check for Rescaling layer baked into the model
    print("\n=== Test 4: What happens with an actual eye-like gradient? ===")
    # Create a simple gradient that mimics an eye
    eye_open = np.zeros((1, 96, 96, 1), dtype=np.float32)
    # Top half = dark (pupil/iris), bottom half = light (sclera)
    for y in range(96):
        if y < 48:
            eye_open[0, y, :, 0] = 50.0 / 255.0   # dark pupil
        else:
            eye_open[0, y, :, 0] = 200.0 / 255.0  # bright sclera

    interpreter.set_tensor(input_details[0]["index"], eye_open)
    interpreter.invoke()
    output = interpreter.get_tensor(output_details[0]["index"])
    softmax = tf.nn.softmax(output[0]).numpy()
    print(f"  Eye-like gradient [0,1]: softmax={softmax}")

    # Same with [-1,1] normalization
    eye_open_n1 = np.zeros((1, 96, 96, 1), dtype=np.float32)
    for y in range(96):
        if y < 48:
            eye_open_n1[0, y, :, 0] = (50.0 - 127.5) / 127.5
        else:
            eye_open_n1[0, y, :, 0] = (200.0 - 127.5) / 127.5

    interpreter.set_tensor(input_details[0]["index"], eye_open_n1)
    interpreter.invoke()
    output = interpreter.get_tensor(output_details[0]["index"])
    softmax = tf.nn.softmax(output[0]).numpy()
    print(f"  Eye-like gradient [-1,1]: softmax={softmax}")

    # Check if model has rescaling layer
    print("\n=== Test 5: Single pixel value sweep to find decision boundary ===")
    print("Finding what input value produces 50/50 split:")
    for norm_type, label in [("01", "[0,1] range"), ("n1", "[-1,1] range")]:
        print(f"\n  {label}:")
        for i in range(21):
            if norm_type == "01":
                val = i * 0.05  # 0, 0.05, ..., 1.0
            else:
                val = -1.0 + i * 0.1  # -1.0, -0.9, ..., 1.0
            test_input = np.full((1, 96, 96, 1), val, dtype=np.float32)
            interpreter.set_tensor(input_details[0]["index"], test_input)
            interpreter.invoke()
            output = interpreter.get_tensor(output_details[0]["index"])
            softmax = tf.nn.softmax(output[0]).numpy()
            print(f"    val={val:+.4f}: open={softmax[0]:.4f} closed={softmax[1]:.4f}")
