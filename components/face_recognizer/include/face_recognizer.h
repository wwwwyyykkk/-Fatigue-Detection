/**
 * face_recognizer.h — 人脸识别 C API
 *
 * MobileFaceNet 预训练 TFLite 模型 + NVS 特征数据库
 * 输入：112×112×3 RGB → 输出 128 维特征向量 → 余弦相似度比对
 *
 * 仅在启动时识别一次（首次检测到人脸），不重复触发。
 * 注册方式：串口命令 enroll 1/2/3 注册驾驶员。
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_FEAT_DIM      128   /* MobileFaceNet 输出特征向量维度 */
#define FACE_MAX_DRIVERS   10    /* NVS 最大注册驾驶员数量 */
#define FACE_DEFAULT_THRESHOLD 0.60f  /* 相似度阈值 60%（多司机场景，降低陌生人误识别） */

/**
 * @brief 人脸识别结果
 */
typedef struct {
    int   driver_id;       /* -1 = 未知, 1+ = 驾驶员 ID */
    float similarity;      /* 余弦相似度 [0..1] */
    bool  is_known;        /* true = driver_id >= 1 */
} face_recognition_result_t;

/**
 * @brief 初始化人脸识别器
 * 加载 MobileFaceNet TFLite 模型，从 PSRAM 分配 tensor arena
 * 同时初始化 NVS
 * @return true 成功
 */
bool face_recognizer_init(void);

/**
 * @brief 从 RGB565 帧的人脸区域提取特征向量
 *
 * @param frame_rgb565  原始帧 RGB565 数据（大端格式）
 * @param frame_w       帧宽度（如 1280）
 * @param frame_h       帧高度（如 720）
 * @param face_box      人脸框 [x1, y1, x2, y2]
 * @param feature       输出 128 维 float 特征向量
 * @return true 成功
 */
bool face_recognizer_extract(
    const uint8_t *frame_rgb565,
    int frame_w, int frame_h,
    const int face_box[4],
    float feature[FACE_FEAT_DIM]);

/**
 * @brief 注册驾驶员特征到 NVS
 *
 * @param driver_id 驾驶员 ID（1..FACE_MAX_DRIVERS）
 * @param feature   128 维特征向量
 * @param name      可选名称（传 NULL 使用默认名）
 * @return true 成功
 */
bool face_recognizer_enroll(
    int driver_id,
    const float feature[FACE_FEAT_DIM],
    const char *name);

/**
 * @brief 识别特征向量对应的驾驶员
 *
 * @param feature   128 维特征向量
 * @param threshold 相似度阈值（推荐 0.6）
 * @return 识别结果
 */
face_recognition_result_t face_recognizer_identify(
    const float feature[FACE_FEAT_DIM],
    float threshold);

/**
 * @brief 删除已注册驾驶员
 * @param driver_id 驾驶员 ID
 * @return true 成功
 */
bool face_recognizer_delete(int driver_id);

/**
 * @brief 获取已注册驾驶员数量
 * @return 数量
 */
int face_recognizer_get_enrolled_count(void);

/**
 * @brief 列出所有已注册驾驶员（写入字符串缓冲）
 * @param out      输出缓冲
 * @param out_size 缓冲大小
 */
void face_recognizer_list_enrolled(char *out, size_t out_size);

/**
 * @brief 释放推理资源
 */
void face_recognizer_deinit(void);

#ifdef __cplusplus
}
#endif
