"""
Check if the v5 model weights look trained or corrupted.

Key question: does this model have properly trained weights, or are they random?
"""
import numpy as np
import tensorflow as tf

tflite_path = r"D:\Fatigue-detection\tools\extracted_v5_model.tflite"

interpreter = tf.lite.Interpreter(model_path=tflite_path)
interpreter.allocate_tensors()

tensor_details = interpreter.get_tensor_details()

print("=== WEIGHT STATISTICS (first 20 tensors) ===")
print(f"{'idx':>4s} {'name':<50s} {'shape':<25s} {'min':>10s} {'max':>10s} {'mean':>10s} {'std':>10s}")
print("-" * 120)

# Check key tensors - especially Conv2D weights and biases
for t in tensor_details:
    name = t['name']
    shape = t['shape']
    dtype = t['dtype']

    # Skip input/output tensors
    if 'serving_default' in name or 'StatefulPartitionedCall' in name:
        continue

    try:
        data = interpreter.get_tensor(t['index'])
        if data.size == 0:
            continue

        dmin = np.min(data)
        dmax = np.max(data)
        dmean = np.mean(data)
        dstd = np.std(data)

        # Check for common issues
        flags = []
        if np.all(data == 0):
            flags.append("ALL_ZEROS!")
        if np.isnan(data).any():
            flags.append("NaN!")
        if np.isinf(data).any():
            flags.append("INF!")
        if dstd < 1e-7 and dmax - dmin < 1e-7:
            flags.append("constant")

        shape_str = str(list(shape))
        flag_str = " *** " + ", ".join(flags) if flags else ""
        print(f"{t['index']:4d} {name:<50s} {shape_str:<25s} {dmin:10.4f} {dmax:10.4f} {dmean:10.6f} {dstd:10.6f}{flag_str}")

    except Exception as e:
        print(f"{t['index']:4d} {name:<50s} ERROR: {e}")

print("\n=== MODEL SUBGRAPH ANALYSIS ===")

# Check the model's op types
from tensorflow.lite.python import schema_py_generated as schema_fb

with open(tflite_path, 'rb') as f:
    buf = f.read()

model = schema_fb.Model.GetRootAsModel(buf, 0)
print(f"Model version: {model.Version()}")
print(f"Description: {model.Description().decode() if model.Description() else 'N/A'}")

# List operator codes
print(f"\nOperator codes ({model.OperatorCodesLength()}):")
for i in range(model.OperatorCodesLength()):
    op = model.OperatorCodes(i)
    builtin = op.BuiltinCode()
    version = op.Version()
    # Map common builtin codes
    op_names = {
        0: "ADD", 1: "AVERAGE_POOL_2D", 2: "CONCATENATION", 3: "CONV_2D",
        4: "DEPTHWISE_CONV_2D", 6: "DEQUANTIZE", 9: "FULLY_CONNECTED",
        14: "LOGISTIC", 15: "LOG_SOFTMAX", 17: "MAX_POOL_2D",
        18: "MUL", 19: "RELU", 20: "RELU_N1_TO_1", 21: "RELU6",
        22: "RESHAPE", 25: "SOFTMAX", 32: "TRANSPOSE_CONV",
        34: "PAD", 41: "CUSTOM", 53: "PRELU", 114: "HARD_SWISH",
        115: "TRANSPOSE", 116: "MEAN", 131: "QUANTIZE",
    }
    op_name = op_names.get(builtin, f"UNKNOWN({builtin})")
    print(f"  {i:3d}: {op_name} (v{version})")

# Check the first subgraph
subgraph = model.Subgraphs(0)
print(f"\nSubgraph 0: {subgraph.InputsLength()} inputs, {subgraph.OutputsLength()} outputs")
print(f"  Tensors: {subgraph.TensorsLength()}, Operators: {subgraph.OperatorsLength()}")

# Check the first few operators
print("\nFirst 10 operators:")
for i in range(min(10, subgraph.OperatorsLength())):
    op = subgraph.Operators(i)
    op_code = model.OperatorCodes(op.OpcodeIndex())
    op_name = {
        0: "ADD", 3: "CONV_2D", 4: "DEPTHWISE_CONV_2D",
        21: "RELU6", 22: "RESHAPE", 114: "HARD_SWISH",
    }.get(op_code.BuiltinCode(), f"OP{op_code.BuiltinCode()}")

    inputs = [op.Inputs(j) for j in range(op.InputsLength())]
    outputs = [op.Outputs(j) for j in range(op.OutputsLength())]
    print(f"  Op {i}: {op_name}  inputs={inputs}  outputs={outputs}")

# Check input tensor details
input_tensor = subgraph.Tensors(subgraph.Inputs(0))
print(f"\nInput tensor: name={input_tensor.Name().decode() if input_tensor.Name() else 'N/A'}")
print(f"  type={input_tensor.Type()}, shape={[input_tensor.Shape(j) for j in range(input_tensor.ShapeLength())]}")
q = input_tensor.Quantization()
if q:
    print(f"  quantization: scale={[q.Scale(j) for j in range(q.ScaleLength())]}, zp={[q.ZeroPoint(j) for j in range(q.ZeroPointLength())]}")

# Output tensor
output_tensor = subgraph.Tensors(subgraph.Outputs(0))
print(f"\nOutput tensor: name={output_tensor.Name().decode() if output_tensor.Name() else 'N/A'}")
print(f"  type={output_tensor.Type()}, shape={[output_tensor.Shape(j) for j in range(output_tensor.ShapeLength())]}")
