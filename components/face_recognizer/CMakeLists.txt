# ── Face Recognizer 组件 ──
# MobileFaceNet TFLite Micro 人脸识别（特征提取 + 身份确认）
#
# 依赖 eye_classifier 复用 TFLite Micro 核心符号。
# 额外编译 MobileFaceNet 需要的但 eye_classifier 未编译的算子。
#
# 模型：ace_embedding.tflite（MobileFaceNet 112×112 RGB → 128维特征向量）
# 对齐：几何关键点估算（从人脸框比例计算5点，无需额外模型）

set(EI_SDK_PATH "../eye_classifier/edge-impulse-sdk")

# MobileFaceNet 额外需要的算子（eye_classifier 未编译的）
# 模型实际只用 6 种 op：Conv2D, DepthwiseConv2D, FullyConnected, PReLU, Mean, L2Normalization
# eye_classifier 已编译 Conv2D, DepthwiseConv2D, FullyConnected
# 以下 3 个是额外需要的
set(MF_EXTRA_KERNEL_SRCS
    "${EI_SDK_PATH}/tensorflow/lite/micro/kernels/prelu.cc"
    "${EI_SDK_PATH}/tensorflow/lite/micro/kernels/prelu_common.cc"
    "${EI_SDK_PATH}/tensorflow/lite/micro/kernels/reduce.cc"
    "${EI_SDK_PATH}/tensorflow/lite/micro/kernels/reduce_common.cc"
    "${EI_SDK_PATH}/tensorflow/lite/micro/kernels/l2norm.cc"
)

idf_component_register(
    SRCS
        "face_recognizer.cpp"
        ${MF_EXTRA_KERNEL_SRCS}
    INCLUDE_DIRS
        "."
        "include"
        "tflite-model"
        "${EI_SDK_PATH}"
        "${EI_SDK_PATH}/CMSIS/Core/Include"
        "${EI_SDK_PATH}/CMSIS/DSP/Include"
        "${EI_SDK_PATH}/third_party/flatbuffers/include"
        "${EI_SDK_PATH}/third_party/gemmlowp"
        "${EI_SDK_PATH}/third_party/ruy"
        "${EI_SDK_PATH}/porting/espressif/ESP-NN/include"
        "${EI_SDK_PATH}/porting/espressif/ESP-NN/src/common"
    PRIV_REQUIRES
        eye_classifier
        nvs_flash
        esp_timer
        heap
        log
)

if(CONFIG_IDF_TARGET_ESP32S3)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -mlongcalls -O3 -Wno-unused-function)
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ESP_NN=0
    EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=0)
