#!/usr/bin/env python3
"""
tflite_to_header.py — Convert TFLite model .tflite to C header byte array

Usage:
    python tflite_to_header.py model.tflite -o model_data.h
    python tflite_to_header.py model.tflite -o model_data.h --name my_model_data --arena 200000
"""

import os
import sys
import argparse

def tflite_to_header(tflite_path, output_path, array_name="g_model_data", arena_size=200000):
    with open(tflite_path, "rb") as f:
        data = f.read()

    size = len(data)
    guard = os.path.basename(output_path).replace(".", "_").upper()

    lines = []
    lines.append(f"#ifndef {guard}")
    lines.append(f"#define {guard}")
    lines.append("")
    lines.append(f"/* Auto-generated from {os.path.basename(tflite_path)} ({size} bytes) */")
    lines.append(f"#define TFLITE_MODEL_ARENA_SIZE {arena_size}")
    lines.append("")
    lines.append(f"const unsigned char {array_name}[] = {{")

    # Write hex bytes, 16 per line
    for i in range(0, size, 16):
        chunk = data[i:i+16]
        hex_str = ", ".join(f"0x{b:02x}" for b in chunk)
        if i + 16 < size:
            lines.append(f"  {hex_str},")
        else:
            lines.append(f"  {hex_str}")
            break  # already written all bytes

    # Ensure last byte ends properly
    if size > 0:
        # The last line may need trailing comma fix
        last_line = lines[-1]
        if not last_line.endswith(",") and not last_line.endswith("{"):
            lines[-1] = last_line + ","  # no, leave as-is for TFLite consumption
            # Actually TFLite just needs the array declared correctly
            pass

    lines.append("};")
    lines.append("")
    lines.append(f"const unsigned int {array_name}_len = {size};")
    lines.append("")
    lines.append(f"#endif /* {guard} */")

    with open(output_path, "w") as f:
        f.write("\n".join(lines) + "\n")

    print(f"[OK] Wrote {size} bytes -> {output_path}")
    print(f"   Array name: {array_name}")
    print(f"   Arena size: {arena_size}")
    print(f"   File size:  {os.path.getsize(output_path)} bytes")

def analyze_tflite(tflite_path):
    """Print model info (input/output shapes, ops) without TF dependency."""
    import struct

    with open(tflite_path, "rb") as f:
        data = f.read()

    print(f"📐 Model: {os.path.basename(tflite_path)} ({len(data)} bytes)")

    # Check magic bytes
    if data[:4] == b'\x20\x00\x00\x00':
        magic = data[4:8]
        if magic == b'TFL3':
            print("   ✓ Valid TFLite model (FlatBuffer + TFL3 magic)")
        else:
            print(f"   ⚠ Unknown magic: {magic.hex()}")
    else:
        # Try TFLite format where first 4 bytes are file size
        print(f"   First 8 bytes: {data[:8].hex()}")

    print()
    print("   To analyze ops, run:")
    print(f"   pip install tflite-support")
    print(f"   python -c \"from tflite_support.metadata_schema_py_generated import *\"")
    print()
    print("   OR use: flatc --raw-binary -t schema.fbs -- {tflite_path} | jq '.operator_codes[]'")

    # Estimate model type
    has_prelu = data.find(b'Prelu') >= 0 or data.find(b'PRELU') >= 0
    has_conv = data.find(b'CONV_2D') >= 0
    has_dw_conv = data.find(b'DEPTHWISE_CONV_2D') >= 0

    print(f"   Detected ops (raw string search):")
    print(f"     CONV_2D:           {'YES' if has_conv else 'NOPE'}")
    print(f"     DEPTHWISE_CONV_2D: {'YES' if has_dw_conv else 'NOPE'}")
    print(f"     PRELU:             {'YES' if has_prelu else 'NOPE'}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Convert TFLite model to C header")
    parser.add_argument("tflite", help="Input .tflite file")
    parser.add_argument("-o", "--output", required=True, help="Output .h file")
    parser.add_argument("--name", default="g_model_data", help="C array name")
    parser.add_argument("--arena", type=int, default=200000, help="TFLite arena size")
    parser.add_argument("--analyze", action="store_true", help="Also print model info")

    args = parser.parse_args()

    if not os.path.exists(args.tflite):
        print(f"[FAIL] File not found: {args.tflite}")
        sys.exit(1)

    if args.analyze:
        analyze_tflite(args.tflite)
        print()

    tflite_to_header(args.tflite, args.output, args.name, args.arena)
