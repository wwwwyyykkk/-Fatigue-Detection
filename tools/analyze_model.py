"""Analyze TFLite model to determine expected input normalization."""
import sys
import numpy as np

try:
    import tensorflow as tf
except ImportError:
    print("ERROR: tensorflow not installed")
    sys.exit(1)

tflite_path = r"D:\Fatigue-detection\Project\components\eye_classifier\tflite-model\tflite_learn_1020434_3.tflite"

interpreter = tf.lite.Interpreter(model_path=tflite_path)
interpreter.allocate_tensors()

input_details = interpreter.get_input_details()
output_details = interpreter.get_output_details()

print("=== INPUT ===")
for d in input_details:
    print(f"  name: {d['name']}")
    print(f"  shape: {d['shape']}")
    print(f"  dtype: {d['dtype']}")
    print(f"  quantization: {d.get('quantization')}")

print("=== OUTPUT ===")
for d in output_details:
    print(f"  name: {d['name']}")
    print(f"  shape: {d['shape']}")
    print(f"  dtype: {d['dtype']}")
    print(f"  quantization: {d.get('quantization')}")

# Inspect first few ops to see if there's a normalization layer
print("\n=== TENSOR DETAILS ===")
tensor_details = interpreter.get_tensor_details()
for t in tensor_details[:10]:
    print(f"  idx={t['index']} name={t['name']} shape={t['shape']} dtype={t['dtype']}")

print("\n=== TESTING INPUT NORMALIZATION ===")

tests = [
    ("zeros [0.0]", np.zeros((1, 96, 96, 1), dtype=np.float32)),
    ("ones [1.0]", np.ones((1, 96, 96, 1), dtype=np.float32)),
    ("half [0.5]", np.full((1, 96, 96, 1), 0.5, dtype=np.float32)),
    ("minus1 [-1.0]", -np.ones((1, 96, 96, 1), dtype=np.float32)),
    ("gray128 [128/255]", np.full((1, 96, 96, 1), 128.0 / 255.0, dtype=np.float32)),
    ("gray0 [0/255]", np.zeros((1, 96, 96, 1), dtype=np.float32)),
    ("gray255 [255/255]", np.ones((1, 96, 96, 1), dtype=np.float32)),
]

for label, test_input in tests:
    interpreter.set_tensor(input_details[0]["index"], test_input)
    interpreter.invoke()
    output = interpreter.get_tensor(output_details[0]["index"])
    softmax = tf.nn.softmax(output[0]).numpy()
    print(f"  {label}: logits={output[0]}  softmax={softmax}")

# Test with raw pixel values (simulating what the board currently does)
print("\n=== RAW PIXEL VALUE TESTS (0-255 range) ===")
for raw_val in [0, 32, 64, 96, 128, 160, 192, 224, 255]:
    test_input = np.full((1, 96, 96, 1), float(raw_val), dtype=np.float32)
    interpreter.set_tensor(input_details[0]["index"], test_input)
    interpreter.invoke()
    output = interpreter.get_tensor(output_details[0]["index"])
    softmax = tf.nn.softmax(output[0]).numpy()
    print(f"  Raw {raw_val}: logits={output[0]}  softmax={softmax}")

# Test: what does the model predict for typical eye images?
# eye open = mostly dark pupil + light sclera (mixed, avg ~100-150)
# eye closed = mostly skin tone (avg ~120-180)
print("\n=== SIMULATED EYE SAMPLES ===")
# Simulate eye open: darker overall (pupil visible)
open_eye = np.full((1, 96, 96, 1), 100.0 / 255.0, dtype=np.float32)
interpreter.set_tensor(input_details[0]["index"], open_eye)
interpreter.invoke()
output = interpreter.get_tensor(output_details[0]["index"])
softmax = tf.nn.softmax(output[0]).numpy()
print(f"  Open eye sim (100/255={100/255:.3f}): softmax={softmax}")

# Simulate eye closed: brighter overall (skin)
closed_eye = np.full((1, 96, 96, 1), 180.0 / 255.0, dtype=np.float32)
interpreter.set_tensor(input_details[0]["index"], closed_eye)
interpreter.invoke()
output = interpreter.get_tensor(output_details[0]["index"])
softmax = tf.nn.softmax(output[0]).numpy()
print(f"  Closed eye sim (180/255={180/255:.3f}): softmax={softmax}")

# Check the model's subgraph operations
print("\n=== MODEL OPS ===")
try:
    # Get op codes from the model
    from tensorflow.lite.python import schema_py_generated as schema_fb
    model = schema_fb.Model.GetRootAsModel(interpreter._interpreter.contents, 0)
    for i in range(model.OperatorCodesLength()):
        op_code = model.OperatorCodes(i)
        print(f"  OpCode {i}: builtin={op_code.BuiltinCode()} version={op_code.Version()}")
except Exception as e:
    print(f"  Could not list ops: {e}")
