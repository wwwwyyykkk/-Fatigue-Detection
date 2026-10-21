"""Quick verify: Rescaling layer exports correctly to TFLite."""
import numpy as np
import tensorflow as tf

# Build minimal test model with Rescaling
model = tf.keras.Sequential([
    tf.keras.layers.InputLayer(input_shape=(96, 96, 1)),
    tf.keras.layers.Rescaling(scale=1./127.5, offset=-1),
    tf.keras.layers.Conv2D(4, 3, padding='same'),
])

# Export to TFLite
converter = tf.lite.TFLiteConverter.from_keras_model(model)
tflite_model = converter.convert()

# Save
with open(r'D:\Fatigue-detection\tools\test_rescaling.tflite', 'wb') as f:
    f.write(tflite_model)

# Analyze ops
interpreter = tf.lite.Interpreter(model_content=tflite_model)
interpreter.allocate_tensors()
input_details = interpreter.get_input_details()
print(f"Input: dtype={input_details[0]['dtype']}, shape={input_details[0]['shape']}")
print(f"Input quantization: {input_details[0].get('quantization')}")

# Test with [0, 255] input
test_input = np.full((1, 96, 96, 1), 127.5, dtype=np.float32)
interpreter.set_tensor(input_details[0]['index'], test_input)
interpreter.invoke()
output = interpreter.get_tensor(interpreter.get_output_details()[0]['index'])

# Verify: 127.5/127.5 - 1 = 0.0 → Conv2D should see ~0.0
print(f"Input 127.5 (mid-gray) → conv output mean: {output.mean():.6f}")
print("Expected: close to 0 (after Rescaling: (127.5/127.5)-1 = 0.0)")

# Verify the op types
from tensorflow.lite.python import schema_py_generated as schema_fb
model_fb = schema_fb.Model.GetRootAsModel(tflite_model, 0)
print(f"\nOps in model ({model_fb.OperatorCodesLength()}):")
for i in range(model_fb.OperatorCodesLength()):
    op_code = model_fb.OperatorCodes(i)
    builtin = op_code.BuiltinCode()
    name = {0: "ADD", 3: "CONV_2D", 18: "MUL", 22: "RESHAPE"}.get(builtin, f"Op{builtin}")
    print(f"  OpCode {i}: {name} (builtin_code={builtin}, version={op_code.Version()})")

# Check if MUL is present
subgraph = model_fb.Subgraphs(0)
print(f"\nSubgraph: {subgraph.OperatorsLength()} operators")
for i in range(subgraph.OperatorsLength()):
    op = subgraph.Operators(i)
    op_code = model_fb.OperatorCodes(op.OpcodeIndex())
    builtin = op_code.BuiltinCode()
    name = {0: "ADD", 3: "CONV_2D", 18: "MUL", 22: "RESHAPE"}.get(builtin, f"Op{builtin}")
    print(f"  Op {i}: {name} (builtin={builtin})")

print("\n✓ If MUL op is present, the op resolver needs AddMul()")
print("✓ If input accepts float32 [0,255], device code is correct")
