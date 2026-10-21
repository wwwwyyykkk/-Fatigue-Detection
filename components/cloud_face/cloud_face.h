#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化云端人脸识别（获取百度 access_token）
 * @param api_key     百度 AI API Key
 * @param secret_key  百度 AI Secret Key
 * @return true 成功
 */
bool cloud_face_init(const char *api_key, const char *secret_key);

/**
 * @brief 云端人脸搜索（1:N 识别）
 *
 * 从 RGB565 帧中裁剪人脸 → JPEG 编码 → Base64 → POST 百度 API
 *
 * @param frame_rgb565  原始 RGB565 帧
 * @param frame_w       帧宽
 * @param frame_h       帧高
 * @param face_box      人脸框 [x1, y1, x2, y2]
 * @param driver_id     输出: 识别到的驾驶员 ID（1+），未识别返回 -1
 * @param confidence    输出: 置信度 0-100
 * @param timeout_ms    HTTP 超时 (建议 3000ms)
 * @return true  云端识别成功（driver_id/confidence 有效）
 * @return false 网络失败/超时 → 调用方应降级到本地方案
 */
bool cloud_face_identify(
    const uint8_t *frame_rgb565, int frame_w, int frame_h,
    const int face_box[4],
    int *driver_id, float *confidence,
    int timeout_ms);

/**
 * @brief 云端人脸注册（将人脸注册到百度人脸库）
 *
 * @param frame_rgb565  原始 RGB565 帧
 * @param frame_w       帧宽
 * @param frame_h       帧高
 * @param face_box      人脸框 [x1, y1, x2, y2]
 * @param driver_id     驾驶员 ID（1+），生成 user_id = "driver_N"
 * @param timeout_ms    HTTP 超时
 * @return true  注册成功
 */
bool cloud_face_enroll(
    const uint8_t *frame_rgb565, int frame_w, int frame_h,
    const int face_box[4],
    int driver_id, int timeout_ms);

/**
 * @brief 释放资源
 */
void cloud_face_deinit(void);

#ifdef __cplusplus
}
#endif