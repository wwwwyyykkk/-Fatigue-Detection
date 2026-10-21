# ── eYe Classifier 组件 ──
# Edge Impulse 生成的 TFLite Micro 眼部分类器
#
# ESP-NN S3 汇编在 INT8 路径有 bug（输出永远饱和 [127,-128]），
# 改用 ESP-NN C ANSI 优化实现（不快但正确）。

# ESP-NN ANSI C 优化源文件
set(ESP_NN_ANSI_SRCS
    "edge-impulse-sdk/porting/espressif/ESP-NN/src/activation_functions/esp_nn_relu_ansi.c"
    "edge-impulse-sdk/porting/espressif/ESP-NN/src/basic_math/esp_nn_add_ansi.c"
    "edge-impulse-sdk/porting/espressif/ESP-NN/src/basic_math/esp_nn_mul_ansi.c"
    "edge-impulse-sdk/porting/espressif/ESP-NN/src/convolution/esp_nn_conv_ansi.c"
    "edge-impulse-sdk/porting/espressif/ESP-NN/src/convolution/esp_nn_conv_opt.c"
    "edge-impulse-sdk/porting/espressif/ESP-NN/src/convolution/esp_nn_depthwise_conv_ansi.c"
    "edge-impulse-sdk/porting/espressif/ESP-NN/src/convolution/esp_nn_depthwise_conv_opt.c"
    "edge-impulse-sdk/porting/espressif/ESP-NN/src/fully_connected/esp_nn_fully_connected_ansi.c"
    "edge-impulse-sdk/porting/espressif/ESP-NN/src/softmax/esp_nn_softmax_ansi.c"
    "edge-impulse-sdk/porting/espressif/ESP-NN/src/softmax/esp_nn_softmax_opt.c"
    "edge-impulse-sdk/porting/espressif/ESP-NN/src/pooling/esp_nn_avg_pool_ansi.c"
    "edge-impulse-sdk/porting/espressif/ESP-NN/src/pooling/esp_nn_max_pool_ansi.c"
)

# TFLite Micro 核心基础文件
set(TFLITE_CORE_SRCS
    "edge-impulse-sdk/tensorflow/lite/micro/micro_interpreter.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/micro_allocator.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/micro_context.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/fake_micro_context.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/flatbuffer_conversions_bridge.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/flatbuffer_utils.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/micro_utils.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/micro_log.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/micro_graph.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/micro_profiler.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/memory_planner/greedy_memory_planner.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/memory_planner/linear_memory_planner.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/micro_allocation_info.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/micro_string.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/micro_time.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/micro_resource_variable.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/system_setup.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/micro_error_reporter.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/memory_helpers.cc"
    "edge-impulse-sdk/tensorflow/lite/c/common.c"
    "edge-impulse-sdk/tensorflow/lite/core/api/common.cc"
    "edge-impulse-sdk/tensorflow/lite/core/api/error_reporter.cc"
    "edge-impulse-sdk/tensorflow/lite/core/api/flatbuffer_conversions.cc"
    "edge-impulse-sdk/tensorflow/lite/core/api/op_resolver.cc"
    "edge-impulse-sdk/tensorflow/lite/core/api/tensor_utils.cc"
    "edge-impulse-sdk/tensorflow/lite/kernels/internal/portable_tensor_utils.cc"
    "edge-impulse-sdk/tensorflow/lite/kernels/internal/quantization_util.cc"
    "edge-impulse-sdk/tensorflow/lite/kernels/internal/reference_portable_tensor_utils.cc"
    "edge-impulse-sdk/tensorflow/lite/kernels/internal/tensor_utils.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/single_arena_buffer_allocator.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/non_persistent_arena_buffer_allocator.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/persistent_arena_buffer_allocator.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/op_resolver_bridge.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/schema_utils.cc"
    "edge-impulse-sdk/tensorflow/lite/kernels/kernel_util_lite.cc"
)

# TFLite Micro 算子
set(TFLITE_KERNEL_SRCS
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/add.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/add_common.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/conv.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/conv_common.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/depthwise_conv.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/depthwise_conv_common.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/fully_connected.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/fully_connected_common.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/reshape.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/softmax.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/softmax_common.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/quantize.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/quantize_common.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/activations.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/activations_common.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/pad.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/pooling.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/pooling_common.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/mul.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/mul_common.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/kernel_util_micro.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/kernel_runner.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/scratch_buffers.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/scratch_buf_mgr.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/ethosu.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/dequantize.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/dequantize_common.cc"
)

idf_component_register(
    SRCS
        "eye_classifier_infer.cpp"
        "edge-impulse-sdk/porting/esp-idf/ei_classifier_porting.cpp"
        "edge-impulse-sdk/porting/espressif/debug_log.cpp"
        ${ESP_NN_ANSI_SRCS}
        # ESP-NN S3 汇编在 INT8 路径有 bug，不编译
        ${TFLITE_CORE_SRCS}
        ${TFLITE_KERNEL_SRCS}
    INCLUDE_DIRS
        "."
        "tflite-model"
        "model-parameters"
        "edge-impulse-sdk"
        "edge-impulse-sdk/CMSIS/Core/Include"
        "edge-impulse-sdk/CMSIS/DSP/Include"
        "edge-impulse-sdk/third_party/flatbuffers/include"
        "edge-impulse-sdk/third_party/gemmlowp"
        "edge-impulse-sdk/third_party/ruy"
        "edge-impulse-sdk/porting/espressif/ESP-NN/include"
        "edge-impulse-sdk/porting/espressif/ESP-NN/src/common"
    PRIV_REQUIRES
        esp_timer
        heap
        log
)

if(CONFIG_IDF_TARGET_ESP32S3)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -mlongcalls -O3 -Wno-unused-function)
endif()

# ESP-NN 临时禁用，用 reference ops 排除 INT8 饱和问题
# 如果 reference ops 正确，说明问题在 ESP-NN（包括 ANSI C）
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ESP_NN=0
    EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=0
    EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN_S3=0)
