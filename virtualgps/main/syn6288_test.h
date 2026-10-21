/**
 * syn6288_test.h - SYN6288 硬件诊断工具
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 运行完整的 SYN6288 硬件诊断流程
 *
 * 检查项：
 *   1. GPIO 配置和电平
 *   2. 发送测试帧并监控 BUSY 信号
 *   3. 输出硬件检查清单
 */
void syn6288_run_diagnostics(void);

#ifdef __cplusplus
}
#endif
