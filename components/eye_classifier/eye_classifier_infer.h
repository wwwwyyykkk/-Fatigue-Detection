/**
 * eye_classifier_infer.h — 眼部分类器推理 C API
 *
 * 供 main 组件调用，无需直接接触 TFLite / INCBIN 文件。
 * 实现在 eye_classifier 组件内部，确保 INCBIN 相对路径正确。
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 TFLite 眼部分类器（从 PSRAM 分配 tensor arena）
 * @return true 成功, false 失败
 */
bool eye_classifier_init(void);

/**
 * @brief 对 96×96 灰度图执行推理
 * @param gray96       输入灰度图 (9216 bytes)
 * @param eye_open_p   [输出] 睁眼概率 [0..1]
 * @param eye_closed_p [输出] 闭眼概率 [0..1]
 * @return true 推理成功
 */
bool eye_classifier_predict(const uint8_t gray96[96 * 96],
                            float *eye_open_p,
                            float *eye_closed_p);

/**
 * @brief 释放 tensor arena（一般不调用，开机申请用到关机）
 */
void eye_classifier_deinit(void);

#ifdef __cplusplus
}
#endif
