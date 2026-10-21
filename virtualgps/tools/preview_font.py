"""Preview generated Chinese font as ASCII art"""
import re
import sys

# Read as bytes to avoid encoding issues
with open("chinese_font.h", "rb") as f:
    raw = f.read()

# Find font data blocks: static const uint8_t XXXX[32] = {...}
pattern = rb"static const uint8_t (\w+)\[32\] = \{([^}]+)\}"
for m in re.finditer(pattern, raw):
    var_name = m.group(1).decode()
    data_bytes = re.findall(rb"0x([0-9A-F]{2})", m.group(2))
    data = [int(b, 16) for b in data_bytes]
    if len(data) < 32:
        continue
    print(f"Character: {var_name}")
    for row in range(16):
        bits = (data[row*2] << 8) | data[row*2+1]
        line = ""
        for col in range(16):
            line += "##" if (bits & (1 << (15 - col))) else "  "
        print(line)
    print()
    break  # Just show first one
