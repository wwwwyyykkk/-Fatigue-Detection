/**
 * syn6288_test.c - SYN6288 诊断工具兼容层
 *
 * 实际诊断功能已迁移到 syn6288.c:
 *   - syn6288_run_diagnostics()  → 完整诊断（GPIO + 测试帧 + 硬件清单）
 *   - syn6288_scan_baud_rate()   → 波特率扫描（自动探测模块波特率）
 *
 * 串口命令:
 *   test_syn  → 运行完整诊断
 *   scan_baud → 扫描波特率
 *   say_hi    → 发送测试语音
 *
 * 此文件仅作为编译占位，所有实现见 syn6288.c
 */
#include "syn6288_test.h"
