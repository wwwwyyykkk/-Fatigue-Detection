/** mic_test.h — USB mic feasibility test */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 开机初始化 UAC（摄像头打开前调用，麦克风持续可用） */
void mic_host_init(void);

/* 串口命令触发：采集测试 */
int mic_test_run(void);

#ifdef __cplusplus
}
#endif
