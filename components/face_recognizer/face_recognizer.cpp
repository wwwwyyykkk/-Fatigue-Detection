/**
 * face_recognizer.cpp — MobileFaceNet 人脸识别实现
 *
 * 依赖 eye_classifier 组件复用的 TFLite Micro 符号（不重复编译 SDK）
 * 创建独立的 interpreter 实例（MobileFaceNet 模型）
 *
 * MobileFaceNet ace_embedding.tflite:
 *   输入: 112 x 112 x 3 RGB, INT8 quantization
 *   输出: 128 维 float32 特征向量 (L2 normalized)
 */

#include "face_recognizer.h"

#include <string.h>
#include <math.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

/* TFLite Micro headers — 通过 eye_classifier 的 INCLUDE_DIRS 传递 */
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"

/* MobileFaceNet 模型数据 */
#include "ace_embedding_model_data.h"

static const char *TAG = "FaceRec";

/* ── 全局状态 ──────────────────────────────────────────────── */
static tflite::MicroInterpreter *s_interpreter = nullptr;
static TfLiteTensor *s_input  = nullptr;
static TfLiteTensor *s_output = nullptr;
static uint8_t *s_tensor_arena = nullptr;

/* ── NVS ──────────────────────────────────────────────────── */
#define NVS_NAMESPACE  "face_db"
#define KEY_COUNT      "driver_cnt"

/* ── 读取 RGB565 像素为 RGB 三元组 ──────────────────────────── */
typedef struct { uint8_t r, g, b; } rgb_t;

static inline rgb_t read_rgb565(const uint8_t *buf, int stride, int x, int y)
{
    int idx = (y * stride + x) * 2;
    uint16_t p = ((uint16_t)buf[idx] << 8) | buf[idx + 1];
    rgb_t res;
    int r5 = (p >> 11) & 0x1F;
    int g6 = (p >>  5) & 0x3F;
    int b5 =  p        & 0x1F;
    res.r = (r5 << 3) | (r5 >> 2);
    res.g = (g6 << 2) | (g6 >> 4);
    res.b = (b5 << 3) | (b5 >> 2);
    return res;
}

/* ── L2 归一化 ────────────────────────────────────────────── */
static void l2_normalize(float *feat, int dim)
{
    float sum = 0.0f;
    for (int i = 0; i < dim; i++) sum += feat[i] * feat[i];
    float norm = sqrtf(sum);
    if (norm > 1e-6f)
        for (int i = 0; i < dim; i++) feat[i] /= norm;
}

/* ── 双线性插值采样 ──────────────────────────────────────── */
static inline void bilinear_sample_rgb(
    const uint8_t *buf, int sw, int h,
    float sx, float sy,
    uint8_t out_rgb[3])
{
    int x0 = (int)sx, y0 = (int)sy;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    int x1 = x0 + 1, y1 = y0 + 1;
    if (x1 >= sw) x1 = sw - 1;
    if (y1 >= h)  y1 = h - 1;

    float fx = sx - x0;
    float fy = sy - y0;

    rgb_t p00 = read_rgb565(buf, sw, x0, y0);
    rgb_t p10 = read_rgb565(buf, sw, x1, y0);
    rgb_t p01 = read_rgb565(buf, sw, x0, y1);
    rgb_t p11 = read_rgb565(buf, sw, x1, y1);

    for (int c = 0; c < 3; c++) {
        uint8_t v00 = ((uint8_t *)&p00)[c];
        uint8_t v10 = ((uint8_t *)&p10)[c];
        uint8_t v01 = ((uint8_t *)&p01)[c];
        uint8_t v11 = ((uint8_t *)&p11)[c];
        float top = v00 * (1.0f - fx) + v10 * fx;
        float bot = v01 * (1.0f - fx) + v11 * fx;
        float val = top * (1.0f - fy) + bot * fy;
        if (val < 0) val = 0;
        if (val > 255) val = 255;
        out_rgb[c] = (uint8_t)val;
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  人脸区域裁剪缩放至 112×112 RGB（带 padding）
 * ═══════════════════════════════════════════════════════════════ */
static void crop_face_rgb112(
    const uint8_t *frame, int fw, int fh,
    const int box[4],
    uint8_t out[112 * 112 * 3])
{
    int x1 = box[0], y1 = box[1], x2 = box[2], y2 = box[3];
    int w = x2 - x1, h = y2 - y1;

    /* expand 20% */
    int px = w / 5, py = h / 5;
    if (x1 - px > 0) x1 -= px;
    if (y1 - py > 0) y1 -= py;
    if (x2 + px < fw) x2 += px;
    if (y2 + py < fh) y2 += py;

    int rw = x2 - x1, rh = y2 - y1;
    if (rw < 8) rw = 8;
    if (rh < 8) rh = 8;

    const int DST = 112;
    for (int dy = 0; dy < DST; dy++) {
        for (int dx = 0; dx < DST; dx++) {
            float sx = x1 + (float)dx / DST * rw;
            float sy = y1 + (float)dy / DST * rh;
            if (sx < 0) sx = 0;
            if (sx >= fw - 1) sx = fw - 2;
            if (sy < 0) sy = 0;
            if (sy >= fh - 1) sy = fh - 2;
            bilinear_sample_rgb(frame, fw, fh, sx, sy, &out[(dy * DST + dx) * 3]);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  几何关键点估算
 * ═══════════════════════════════════════════════════════════════ */
static void estimate_landmarks(
    const int face_box[4],
    float lmk[10])
{
    int x = face_box[0], y = face_box[1];
    float w = (float)(face_box[2] - face_box[0]);
    float h = (float)(face_box[3] - face_box[1]);

    /* 标准 112×112 模板 5 点坐标映射到人脸框 (ArcFace canonical) */
    const float std[10] = {
        38.2946f, 51.6963f,   /* 左眼 */
        73.5318f, 51.5014f,   /* 右眼 */
        56.0252f, 71.7366f,   /* 鼻 */
        41.5493f, 92.3655f,   /* 左嘴角 */
        70.7299f, 92.2041f,   /* 右嘴角 */
    };
    for (int i = 0; i < 10; i += 2) {
        lmk[i]   = x + std[i]   / 112.0f * w;
        lmk[i+1] = y + std[i+1] / 112.0f * h;
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  仿射变换对齐
 * ═══════════════════════════════════════════════════════════════ */
static void warp_affine_align(
    const uint8_t *frame, int fw, int fh,
    const float lmk[10],
    uint8_t aligned[112 * 112 * 3])
{
    const float std[10] = {
        38.2946f, 51.6963f,   /* 左眼 */
        73.5318f, 51.5014f,   /* 右眼 */
        56.0252f, 71.7366f,   /* 鼻 */
        41.5493f, 92.3655f,   /* 左嘴角 */
        70.7299f, 92.2041f,   /* 右嘴角 */
    };

    float src_dx = lmk[2] - lmk[0], src_dy = lmk[3] - lmk[1];
    float src_dist = sqrtf(src_dx * src_dx + src_dy * src_dy);
    float dst_dx = std[2] - std[0], dst_dy = std[3] - std[1];
    float dst_dist = sqrtf(dst_dx * dst_dx + dst_dy * dst_dy);
    float scale = (src_dist > 1e-6f) ? dst_dist / src_dist : 1.0f;

    float angle = atan2f(src_dy, src_dx) - atan2f(dst_dy, dst_dx);
    float cos_a = cosf(angle), sin_a = sinf(angle);

    float cx = lmk[0], cy = lmk[1];  /* 左眼参考点 */
    float dx0 = std[0], dy0 = std[1];

    for (int dy = 0; dy < 112; dy++) {
        for (int dx = 0; dx < 112; dx++) {
            float tx = dx - dx0, ty = dy - dy0;
            float sx = (cos_a * tx - sin_a * ty) / scale + cx;
            float sy = (sin_a * tx + cos_a * ty) / scale + cy;
            if (sx < 0) sx = 0;
            if (sx >= fw - 1) sx = fw - 2;
            if (sy < 0) sy = 0;
            if (sy >= fh - 1) sy = fh - 2;
            bilinear_sample_rgb(frame, fw, fh, sx, sy, &aligned[(dy * 112 + dx) * 3]);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  初始化
 * ═══════════════════════════════════════════════════════════════ */

bool face_recognizer_init(void)
{
    ESP_LOGI(TAG, "=== face_recognizer init ===");
    ESP_LOGI(TAG, "Free internal: %lu KB  PSRAM: %lu KB",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);

    /* 1. load model */
    const tflite::Model *model = tflite::GetModel(ace_embedding_model_data);
    if (!model) { ESP_LOGE(TAG, "GetModel failed"); return false; }
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Model v%d != expected v%d",
                 (int)model->version(), (int)TFLITE_SCHEMA_VERSION);
        return false;
    }
    ESP_LOGI(TAG, "Model v%d OK", (int)model->version());

    /* 2. arena in PSRAM */
    size_t arena_size = TFLITE_MODEL_ARENA_SIZE;
    ESP_LOGI(TAG, "Arena: %zu bytes (%zu KB)", arena_size, arena_size / 1024);

    s_tensor_arena = (uint8_t *)heap_caps_aligned_alloc(
        16, arena_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tensor_arena) {
        ESP_LOGW(TAG, "PSRAM failed, fallback to DRAM...");
        s_tensor_arena = (uint8_t *)heap_caps_aligned_alloc(
            16, arena_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_tensor_arena) {
        ESP_LOGE(TAG, "Arena alloc failed (%zu bytes)", arena_size);
        return false;
    }
    bool is_psram = ((uintptr_t)s_tensor_arena >= 0x3C000000 &&
                     (uintptr_t)s_tensor_arena < 0x3D000000);
    ESP_LOGI(TAG, "Arena @ %p (%s)", s_tensor_arena, is_psram ? "PSRAM" : "DRAM");

    /* 3. op resolver — exact MobileFaceNet ops (6 total) */
    static tflite::MicroMutableOpResolver<6> resolver;
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddFullyConnected();
    resolver.AddPrelu();
    resolver.AddMean();
    resolver.AddL2Normalization();
    ESP_LOGI(TAG, "OpResolver done");

    /* 4. interpreter */
    s_interpreter = new tflite::MicroInterpreter(
        model, resolver, s_tensor_arena, arena_size, nullptr, nullptr);
    if (!s_interpreter) { ESP_LOGE(TAG, "Interpreter alloc fail"); goto fail; }

    {
        ESP_LOGI(TAG, "Calling AllocateTensors (arena=%zu)...", arena_size);
        TfLiteStatus st = s_interpreter->AllocateTensors(true);
        if (st != kTfLiteOk) {
            ESP_LOGE(TAG, "AllocateTensors fail: %d", (int)st);
            ESP_LOGI(TAG, "arena=%zu largest_psram=%zu", arena_size,
                     heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            goto fail;
        }
        size_t used = s_interpreter->arena_used_bytes();
        ESP_LOGI(TAG, "Tensors allocated (arena used: %zu / %zu)", used, arena_size);
    }

    s_input  = s_interpreter->input(0);
    s_output = s_interpreter->output(0);
    if (!s_input || !s_output) { ESP_LOGE(TAG, "Null tensor"); goto fail; }

    ESP_LOGI(TAG, "Input {%d, %d, %d} scale=%f zp=%d",
             s_input->dims->data[1], s_input->dims->data[2],
             s_input->dims->data[3],
             s_input->params.scale, s_input->params.zero_point);
    ESP_LOGI(TAG, "Output {%d} scale=%f zp=%d",
             s_output->dims->data[1],
             s_output->params.scale, s_output->params.zero_point);

    /* 5. NVS flash init (may already be initialized by main, that's fine) */
    {
        esp_err_t nvs_err = nvs_flash_init();
        if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGW(TAG, "NVS corrupt or version mismatch, erasing...");
            ESP_ERROR_CHECK(nvs_flash_erase());
            nvs_err = nvs_flash_init();
        }
        if (nvs_err != ESP_OK && nvs_err != ESP_ERR_NVS_INVALID_HANDLE) {
            ESP_LOGW(TAG, "NVS init result: %s (may already be done by main)", esp_err_to_name(nvs_err));
        }
    }

    /* 6. self-test (heap buffer — 112*112*3=37KB overflows stack) */
    {
        uint8_t *test = (uint8_t *)heap_caps_malloc(112 * 112 * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (test) {
            memset(test, 128, 112 * 112 * 3);
            float feat[FACE_FEAT_DIM];
            bool ok = face_recognizer_extract(test, 112, 112,
                                               (int[]){0, 0, 112, 112}, feat);
            if (ok) {
                float sq = 0;
                for (int i = 0; i < FACE_FEAT_DIM; i++) sq += feat[i] * feat[i];
                float norm = sqrtf(sq);
                ESP_LOGI(TAG, "Self-test: embed norm=%.3f", norm);
            }
            heap_caps_free(test);
        }
    }

    {
        int enrolled = face_recognizer_get_enrolled_count();
        ESP_LOGI(TAG, "=== face_recognizer OK (drivers=%d) ===", enrolled);
    }
    return true;

fail:
    if (s_interpreter) {
        /* 先把 interpreter 标记为已析构，避免内部二次析构 */
        /* 如果 AllocateTensors 失败，MicroInterpreter 析构会崩溃。
         * 直接 free arena，不调 delete。 */
        s_interpreter = nullptr;
    }
    if (s_tensor_arena) { heap_caps_free(s_tensor_arena); s_tensor_arena = nullptr; }
    s_input = s_output = nullptr;
    return false;
}

/* ═══════════════════════════════════════════════════════════════
 *  特征提取
 * ═══════════════════════════════════════════════════════════════ */

bool face_recognizer_extract(
    const uint8_t *frame_rgb565,
    int frame_w, int frame_h,
    const int face_box[4],
    float feature[FACE_FEAT_DIM])
{
    if (!s_interpreter || !s_input || !s_output) {
        ESP_LOGE(TAG, "Not initialized"); return false;
    }

    int64_t t0 = esp_timer_get_time();

    /* 1. Estimate 5 facial landmarks from face box, then affine-align to 112x112
     *    MobileFaceNet requires spatially-aligned faces — without this, cross-frame
     *    embeddings drift and recognition accuracy degrades. */
    float lmk[10];
    estimate_landmarks(face_box, lmk);

    uint8_t *aligned = (uint8_t *)heap_caps_malloc(112 * 112 * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!aligned) return false;

    warp_affine_align(frame_rgb565, frame_w, frame_h, lmk, aligned);

    int64_t t1 = esp_timer_get_time();

    /* 2. input: support both INT8 quantized and FLOAT32 */
    size_t n = 112 * 112 * 3;
    if (s_input->type == kTfLiteInt8) {
        float in_s = s_input->params.scale;
        int in_zp = s_input->params.zero_point;
        for (size_t i = 0; i < n; i++) {
            float real = aligned[i] / 255.0f;
            int q = (int)(real / in_s + 0.5f) + in_zp;
            if (q < -128) q = -128;
            if (q > 127) q = 127;
            s_input->data.int8[i] = (int8_t)q;
        }
    } else {
        /* FLOAT32 input: normalize [0,255] to [-1,1] (MobileFaceNet standard) */
        for (size_t i = 0; i < n; i++) {
            s_input->data.f[i] = (aligned[i] / 127.5f) - 1.0f;
        }
        static bool s_log_norm = false;
        if (!s_log_norm) {
            s_log_norm = true;
            ESP_LOGI(TAG, "FLOAT32 model: input norm [-1,1], first pixel=%f", s_input->data.f[0]);
        }
    }
    heap_caps_free(aligned);

    /* 4. inference */
    if (s_interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGE(TAG, "Invoke failed"); return false;
    }
    int64_t t2 = esp_timer_get_time();

    /* 5. output: use data.f regardless of type (TFLite Micro handles conversion) */
    int out_n = s_output->dims->data[1];
    if (out_n > FACE_FEAT_DIM) out_n = FACE_FEAT_DIM;
    for (int i = 0; i < out_n; i++)
        feature[i] = s_output->data.f[i];
    for (int i = out_n; i < FACE_FEAT_DIM; i++) feature[i] = 0;

    /* Diagnose: always check feature sum on first 3 calls */
    {
        static int s_diag_cnt = 0;
        if (s_diag_cnt < 3) {
            s_diag_cnt++;
            float sum = 0, sum_sq = 0;
            for (int i = 0; i < 128; i++) { sum += fabs(feature[i]); sum_sq += feature[i] * feature[i]; }
            ESP_LOGI(TAG, "DIAG#%d: out_type=%d sum_abs=%.4f sum_sq=%.4f norm=%.4f "
                     "first8=[%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f]",
                     s_diag_cnt, s_output->type, sum, sum_sq, sqrtf(sum_sq),
                     feature[0], feature[1], feature[2], feature[3],
                     feature[4], feature[5], feature[6], feature[7]);
        }
    }

    /* 6. L2 归一化 */
    l2_normalize(feature, FACE_FEAT_DIM);
    int64_t t3 = esp_timer_get_time();

    static bool s_log_once = false;
    if (!s_log_once) {
        s_log_once = true;
        ESP_LOGI(TAG, "Extract: align+quant=%lld ms  invoke=%lld ms  post=%lld ms",
                 (t1 - t0) / 1000, (t2 - t1) / 1000, (t3 - t2) / 1000);
    }

    return true;
}

/* ═══════════════════════════════════════════════════════════════
 *  NVS 特征数据库
 * ═══════════════════════════════════════════════════════════════ */

static nvs_handle_t s_nvs = 0;
static bool s_nvs_ok = false;

static bool nvs_open_db(void)
{
    if (s_nvs_ok) return true;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (e == ESP_OK) { s_nvs_ok = true; return true; }
    ESP_LOGE(TAG, "NVS open fail: %s", esp_err_to_name(e));
    return false;
}

static void nvs_close_db(void)
{
    if (s_nvs_ok) { nvs_close(s_nvs); s_nvs_ok = false; }
}

bool face_recognizer_enroll(int id, const float f[FACE_FEAT_DIM], const char *name)
{
    if (id < 1 || id > FACE_MAX_DRIVERS) return false;
    if (!nvs_open_db()) return false;

    char kf[16], kn[16];
    snprintf(kf, sizeof(kf), "f_%d", id);
    snprintf(kn, sizeof(kn), "n_%d", id);

    nvs_set_blob(s_nvs, kf, f, FACE_FEAT_DIM * sizeof(float));
    char buf[32];
    if (!name) { snprintf(buf, sizeof(buf), "驾驶员 #%d", id); name = buf; }
    nvs_set_str(s_nvs, kn, name);
    /* Fix: initialize cnt=0 so nvs_get_i32 failure leaves it as 0 */
    int32_t cnt = 0;
    nvs_get_i32(s_nvs, KEY_COUNT, &cnt);
    if (id > cnt) nvs_set_i32(s_nvs, KEY_COUNT, (int32_t)id);
    nvs_commit(s_nvs);
    nvs_close_db();
    ESP_LOGI(TAG, "Enrolled driver #%d (total drivers=%d)", id, id > cnt ? id : cnt);
    return true;
}

face_recognition_result_t face_recognizer_identify(
    const float f[FACE_FEAT_DIM], float thr)
{
    face_recognition_result_t r = { -1, 0, false };
    if (!nvs_open_db()) return r;

    int32_t cnt = 0;
    if (nvs_get_i32(s_nvs, KEY_COUNT, &cnt) != ESP_OK || cnt < 1) {
        nvs_close_db(); return r;
    }
    float best_sim = 0; int best_id = -1;
    for (int id = 1; id <= cnt; id++) {
        char kf[16]; snprintf(kf, sizeof(kf), "f_%d", id);
        float stored[FACE_FEAT_DIM];
        size_t sz = sizeof(stored);
        if (nvs_get_blob(s_nvs, kf, stored, &sz) != ESP_OK) continue;
        float sim = 0;
        for (int i = 0; i < FACE_FEAT_DIM; i++) sim += f[i] * stored[i];
        if (sim < 0.0f) sim = 0.0f;
        if (sim > 1.0f) sim = 1.0f;
        if (sim > best_sim) { best_sim = sim; best_id = id; }
        /* DIAG: 对比存储特征 vs 当前特征前 8 维 + 完整向量差异 */
        ESP_LOGI(TAG, "IDENT[%d]: sim=%.3f cur[:8]=[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f]",
                 id, sim, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
        ESP_LOGI(TAG, "IDENT[%d]:     stored[:8]=[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f]",
                 id, stored[0], stored[1], stored[2], stored[3],
                 stored[4], stored[5], stored[6], stored[7]);
        int same = 0;
        for (int i = 0; i < FACE_FEAT_DIM; i++)
            if (fabsf(f[i] - stored[i]) < 0.05f) same++;
        ESP_LOGI(TAG, "IDENT[%d]: dims_close(<0.05)=%d/128  (128=完全相同 0=完全不同)",
                 id, same);
    }
    nvs_close_db();
    if (best_sim >= thr && best_id >= 1) {
        r.driver_id = best_id; r.similarity = best_sim; r.is_known = true;
    } else {
        r.similarity = best_sim;
    }
    return r;
}

bool face_recognizer_delete(int id)
{
    if (id < 1 || id > FACE_MAX_DRIVERS || !nvs_open_db()) return false;
    char kf[16], kn[16];
    snprintf(kf, sizeof(kf), "f_%d", id);
    snprintf(kn, sizeof(kn), "n_%d", id);
    nvs_erase_key(s_nvs, kf); nvs_erase_key(s_nvs, kn);
    /* Recalculate KEY_COUNT: scan remaining drivers to find highest ID */
    int32_t new_cnt = 0;
    for (int i = 1; i <= FACE_MAX_DRIVERS; i++) {
        char test[16]; snprintf(test, sizeof(test), "f_%d", i);
        size_t dummy = 0;
        if (nvs_get_blob(s_nvs, test, NULL, &dummy) == ESP_OK) new_cnt = (int32_t)i;
    }
    nvs_set_i32(s_nvs, KEY_COUNT, new_cnt);
    nvs_commit(s_nvs); nvs_close_db();
    ESP_LOGI(TAG, "Deleted driver #%d (remaining: %ld)", id, (long)new_cnt);
    return true;
}

int face_recognizer_get_enrolled_count(void)
{
    if (!nvs_open_db()) return 0;
    /* Count actual existing keys, not just KEY_COUNT (which may be stale) */
    int32_t c = 0;
    for (int i = 1; i <= FACE_MAX_DRIVERS; i++) {
        char test[16]; snprintf(test, sizeof(test), "f_%d", i);
        size_t dummy = 0;
        if (nvs_get_blob(s_nvs, test, NULL, &dummy) == ESP_OK) c = (int32_t)i;
    }
    nvs_close_db(); return (int)c;
}

void face_recognizer_list_enrolled(char *out, size_t sz)
{
    out[0] = 0;
    if (!nvs_open_db()) return;
    size_t pos = 0;
    for (int id = 1; id <= FACE_MAX_DRIVERS; id++) {
        char kn[16]; snprintf(kn, sizeof(kn), "n_%d", id);
        char name[64] = {0}; size_t ns = sizeof(name);
        /* Only list if key actually exists, regardless of KEY_COUNT */
        if (nvs_get_str(s_nvs, kn, name, &ns) == ESP_OK) {
            int n = snprintf(out + pos, sz - pos, "  #%d: %s\n", id, name);
            if (n > 0 && (size_t)pos + n < sz) pos += n;
        }
    }
    nvs_close_db();
}

void face_recognizer_deinit(void)
{
    if (s_interpreter) { delete s_interpreter; s_interpreter = nullptr; }
    if (s_tensor_arena) { heap_caps_free(s_tensor_arena); s_tensor_arena = nullptr; }
    s_input = s_output = nullptr;
    nvs_close_db();
}
