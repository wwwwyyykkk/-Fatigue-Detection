/**
 * eye_classifier_infer.cpp — 眼部分类器推理实现
 *
 * 放在 eye_classifier 组件内，确保 INCBIN 相对路径正确解析。
 */

#include "eye_classifier_infer.h"
#include <esp_log.h>
#include <esp_heap_caps.h>

/* TFLite Micro 头文件 */
#include "tflite-model/tflite_model_data.h"
#include "tflite-model/tflite-resolver.h"
#include "edge-impulse-sdk/tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "edge-impulse-sdk/tensorflow/lite/micro/micro_interpreter.h"

/* 打印 ESP-NN 宏定义状态 */
#if defined(EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN) && EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN == 1
  #if defined(ESP_NN) && ESP_NN == 1
    #if defined(CONFIG_IDF_TARGET_ESP32S3)
      #pragma message "ESP-NN: ENABLED with S3 assembly optimizations"
    #else
      #pragma message "ESP-NN: ENABLED (generic C opt)"
    #endif
  #else
    #pragma message "ESP-NN: EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=1 but ESP_NN not set!"
  #endif
#else
  #pragma message "ESP-NN: DISABLED (reference implementation)"
#endif
#include "edge-impulse-sdk/tensorflow/lite/schema/schema_generated.h"

static const char *TAG = "EyeClf";

/* 全局推理状态 */
static tflite::MicroInterpreter *s_interpreter = nullptr;
static TfLiteTensor *s_input  = nullptr;
static TfLiteTensor *s_output = nullptr;
static uint8_t *s_tensor_arena = nullptr;

bool eye_classifier_init(void)
{
    ESP_LOGI(TAG, "=== init start ===");
    ESP_LOGI(TAG, "Free internal: %lu KB  PSRAM: %lu KB",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);

    const tflite::Model *model = tflite::GetModel(tflite_model_data);
    if (!model) {
        ESP_LOGE(TAG, "GetModel returned NULL");
        return false;
    }
    ESP_LOGI(TAG, "Model v%d (expected v%d)", (int)model->version(),
             (int)TFLITE_SCHEMA_VERSION);

    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Model schema v%d != expected v%d",
                 model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }

    size_t arena_size = 685504;  /* FLOAT32 v7 model arena (from CLAUDE.md) */
    ESP_LOGI(TAG, "Arena size: %zu (%.1f KB)", arena_size, arena_size / 1024.0f);

    /* 打印各区域最大连续块 */
    size_t largest_int_dma = heap_caps_get_largest_free_block(
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    size_t largest_int = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Largest: DMA-INT=%zu  INT-8BIT=%zu  SPIRAM=%zu (all bytes)",
             largest_int_dma, largest_int, largest_psram);

    /* tensor arena — 放 PSRAM（USB OTG DMA 不能访问 PSRAM，天然防止冲突）
     * 之前放内部 DRAM 时 USB DMA 在 2.3s 推理期间不断写帧缓冲到内部 SRAM，
     * 总线冲突导致 arena 数据损坏 → 输出饱和 [127,-128]。
     * 放入 PSRAM 后 USB DMA 无法触及，TFLite CPU 通过 cache 读取无冲突。
     * 注意：不用 MALLOC_CAP_DMA，避免占用 USB DMA 池（仅 32KB）。
     * 参考：ESP32-S3 TRM §30.3 — USB OTG DMA 只能访问 AHB-Lite 总线上的内存。 */
    bool dma_ok = false;
    s_tensor_arena = (uint8_t *)heap_caps_aligned_alloc(16, arena_size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_tensor_arena) {
        dma_ok = false; /* PSRAM 不在 DMA 地址空间 */
        ESP_LOGI(TAG, "Arena in PSRAM — safe from USB DMA corruption");
    }
    if (!s_tensor_arena) {
        ESP_LOGW(TAG, "PSRAM alloc failed (%zuB, largest_psram=%zuB). "
                 "Falling back to INTERNAL DRAM...",
                 arena_size, largest_psram);
        s_tensor_arena = (uint8_t *)heap_caps_aligned_alloc(16, arena_size,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_tensor_arena) {
        ESP_LOGE(TAG, "All alloc failed for arena (%zu bytes)!", arena_size);
        return false;
    }
    /* ESP32-S3 内存映射：PSRAM 通过 cache 映射在 0x3C000000~0x3CFFFFFF 区域 */
    bool is_psram = ((uintptr_t)s_tensor_arena >= 0x3C000000 &&
                     (uintptr_t)s_tensor_arena < 0x3D000000);
    ESP_LOGI(TAG, "Arena @ %p (%s%s)",
             s_tensor_arena,
             is_psram ? "PSRAM" : "INTERNAL DRAM",
             dma_ok ? " WITH DMA" : "");

    /* op resolver */
    EI_TFLITE_RESOLVER
    ESP_LOGI(TAG, "OpResolver created");

    s_interpreter = new tflite::MicroInterpreter(
        model, resolver, s_tensor_arena, arena_size, nullptr, nullptr);
    if (!s_interpreter) {
        ESP_LOGE(TAG, "MicroInterpreter alloc failed");
        goto fail;
    }
    ESP_LOGI(TAG, "Interpreter @ %p", s_interpreter);

    {
        TfLiteStatus st = s_interpreter->AllocateTensors(true);
        if (st != kTfLiteOk) {
            ESP_LOGE(TAG, "AllocateTensors failed: %d (arena too small? try larger)", (int)st);
            goto fail;
        }
        size_t used = s_interpreter->arena_used_bytes();
        ESP_LOGI(TAG, "Tensors allocated  (arena used: %zu / %zu bytes)",
                 used, arena_size);

        if (used > arena_size * 0.9f) {
            ESP_LOGW(TAG, "Arena nearly full! used=%zu / %zu (%.0f%%)",
                     used, arena_size, 100.0f * used / arena_size);
        }
    }

    s_input  = s_interpreter->input(0);
    s_output = s_interpreter->output(0);
    if (!s_input || !s_output) {
        ESP_LOGE(TAG, "Null tensor after AllocateTensors");
        goto fail;
    }

    ESP_LOGI(TAG, "Input type=%d {%dx%dx%d} scale=%f zp=%d",
             s_input->type,
             s_input->dims->data[1], s_input->dims->data[2],
             s_input->dims->data[3],
             s_input->params.scale, s_input->params.zero_point);
    ESP_LOGI(TAG, "Output type=%d {%d} scale=%f zp=%d",
             s_output->type, s_output->dims->data[1],
             s_output->params.scale, s_output->params.zero_point);

    ESP_LOGI(TAG, "=== init OK ===");

    /* ── 自检：在 USB 启动前跑一次推理，验证模型能否输出非饱和值 ── */
    {
        uint8_t test_pattern[96 * 96];
        /* 用渐变灰度检测模型：左半暗(32)、右半亮(192) */
        for (int y = 0; y < 96; y++) {
            for (int x = 0; x < 96; x++) {
                test_pattern[y * 96 + x] = (x < 48) ? 32 : 192;
            }
        }
        float t_open = 0, t_closed = 0;
        bool self_test_ok = eye_classifier_predict(test_pattern, &t_open, &t_closed);

        /* 手动打印自检结果（不依赖 DIAG 静态变量） */
        ESP_LOGI(TAG, "SELF-TEST: open=%.4f closed=%.4f %s",
                 t_open, t_closed,
                 (t_open > 0.1f && t_closed > 0.1f)
                     ? "✓ model produces varied output"
                     : "? model output saturated — possible model/quantization issue");

        /* 即使自检饱和也继续运行，只看日志 */
    }

    return true;

fail:
    /* 清理已分配的资源 */
    if (s_interpreter) { delete s_interpreter; s_interpreter = nullptr; }
    if (s_tensor_arena) { heap_caps_free(s_tensor_arena); s_tensor_arena = nullptr; }
    s_input = nullptr;
    s_output = nullptr;
    ESP_LOGE(TAG, "=== init FAILED ===");
    return false;
}

bool eye_classifier_predict(const uint8_t gray96[96 * 96],
                            float *eye_open_p,
                            float *eye_closed_p)
{
    if (!s_interpreter || !s_input || !s_output) return false;

    /* ── 输入预处理：自动适配 INT8 / FLOAT32 模型
     * FLOAT32:  灰度 uint8 [0,255] → 归一化 [0,1] → 写入 float tensor
     *           模型内置 Rescaling(2.0, -1) 会在第一层转换为 [-1,1]
     * INT8:     灰度 → 归一化 [0,1] → 量化到 INT8 */
    const bool is_int8 = (s_input->type == kTfLiteInt8);

    if (is_int8) {
        float in_s  = s_input->params.scale;
        int   in_zp = s_input->params.zero_point;
        for (size_t i = 0; i < 96 * 96; i++) {
            float real = gray96[i] / 255.0f;  // [0, 1]
            int q = (int)(real / in_s + 0.5f) + in_zp;
            if (q < -128) q = -128;
            if (q > 127)  q = 127;
            s_input->data.int8[i] = (int8_t)q;
        }
    } else {
        /* FLOAT32 input: normalize [0,255] → [0,1] */
        for (size_t i = 0; i < 96 * 96; i++) {
            s_input->data.f[i] = gray96[i] / 255.0f;
        }
    }

    TfLiteStatus st = s_interpreter->Invoke();
    if (st != kTfLiteOk) return false;

    /* ── 输出解析：自动适配 INT8 / FLOAT32
     * Edge Impulse 标签为字母序：[0]=eye_closed, [1]=eye_open */
    if (is_int8) {
        float out_s  = s_output->params.scale;
        int   out_zp = s_output->params.zero_point;
        *eye_closed_p = (float)(s_output->data.int8[0] - out_zp) * out_s;
        *eye_open_p   = (float)(s_output->data.int8[1] - out_zp) * out_s;
    } else {
        /* FLOAT32 output: raw logits — apply softmax manually */
        float r0 = s_output->data.f[0];   /* eye_closed logit */
        float r1 = s_output->data.f[1];   /* eye_open logit */
        float max_r = (r0 > r1) ? r0 : r1;
        float e0 = expf(r0 - max_r);
        float e1 = expf(r1 - max_r);
        float sum = e0 + e1;
        *eye_closed_p = e0 / sum;
        *eye_open_p   = e1 / sum;
    }

    /* 前 3 帧打印诊断信息 */
    static int s_diag = 0;
    if (s_diag++ < 3) {
        if (is_int8) {
            uint32_t sum = 0, sum_q = 0;
            for (size_t i = 0; i < 96 * 96; i++) {
                sum += gray96[i];
                sum_q += (uint8_t)(s_input->data.int8[i] + 128);
            }
            ESP_LOGI(TAG, "DIAG[%d] (INT8): in_scale=%f in_zp=%d  "
                     "gray_sum=%lu q_sum=%lu  "
                     "out=[%.4f, %.4f]  idx0=%s idx1=%s",
                     s_diag, s_input->params.scale, s_input->params.zero_point,
                     sum, sum_q,
                     *eye_closed_p, *eye_open_p,
                     "eye_closed", "eye_open");
        } else {
            uint32_t sum = 0;
            float sum_f = 0;
            for (size_t i = 0; i < 96 * 96; i++) {
                sum += gray96[i];
                sum_f += s_input->data.f[i];
            }
            ESP_LOGI(TAG, "DIAG[%d] (FLOAT32): gray_sum=%lu input_f_sum=%.1f  "
                     "out=[%.4f, %.4f]  idx0=%s idx1=%s",
                     s_diag, sum, sum_f,
                     *eye_closed_p, *eye_open_p,
                     "eye_closed", "eye_open");
        }
        /* 打印前 20 个像素 */
        char buf[64]; buf[0] = 0;
        for (int i = 0; i < 20; i++) {
            char tmp[8]; sprintf(tmp, "%02x ", gray96[i]);
            strcat(buf, tmp);
        }
        ESP_LOGI(TAG, "DIAG[%d] pixels: %s", s_diag, buf);
    }

    return true;
}

void eye_classifier_deinit(void)
{
    if (s_interpreter) {
        delete s_interpreter;
        s_interpreter = nullptr;
    }
    if (s_tensor_arena) {
        heap_caps_free(s_tensor_arena);
        s_tensor_arena = nullptr;
    }
    s_input  = nullptr;
    s_output = nullptr;
}
