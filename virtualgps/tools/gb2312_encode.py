"""将中文文本转为 SYN6288 所需的 GB2312 字节数组 C 代码。"""
import sys
sys.stdout.reconfigure(encoding='utf-8')

TEXTS = {
    "SYS_BOOT":         "系统初始化完成",
    "OVERSPEED":        "检测到超速，请减速行驶",
    "SERVICE_AREA":     "当前位置为服务区，可以安全停车",
    "EMERGENCY_LANE":   "警告，应急车道禁止停车",
    "DRIVE_3H":         "已连续驾驶三小时",
    "DRIVE_3_5H":       "已连续驾驶三小时三十分钟，建议寻找服务区休息",
    "DRIVE_4H":         "已连续驾驶四小时，请立即停车休息",
    "REST_DONE":        "休息时间已满，可以继续出发",
    "DRIVER_SWITCH":    "司机身份已切换，请重新进行人脸验证",
}

print("// Auto-generated SYN6288 voice frames (GB2312)")
print("#pragma once")
print('#include <stdint.h>')
print()

for name, text in TEXTS.items():
    # GB2312 encode
    gb = text.encode('gb2312')
    hex_bytes = ', '.join(f'0x{b:02X}' for b in gb)
    print(f'// "{text}"')
    print(f'static const uint8_t SYN_VOICE_{name}[] = {{{hex_bytes}}};')
    print(f'static const int SYN_VOICE_{name}_LEN = {len(gb)};')
    print()
