/**
 * face_recognizer_resolver.h — MobileFaceNet TFLite op resolver
 *
 * MobileFaceNet 主要算子：Conv2D, DepthwiseConv2D, FullyConnected,
 * PReLU, Reshape, Pad, Mul, Add, Quantize, Dequantize, AvgPool2D
 */

#pragma once

#include "../eye_classifier/edge-impulse-sdk/tensorflow/lite/micro/kernels/micro_ops.h"

#define MOBILEFACE_RESOLVER static tflite::MicroMutableOpResolver<25> resolver; \
    resolver.AddConv2D(); \
    resolver.AddDepthwiseConv2D(); \
    resolver.AddFullyConnected(); \
    resolver.AddReshape(); \
    resolver.AddSoftmax(); \
    resolver.AddAdd(); \
    resolver.AddMul(); \
    resolver.AddPad(); \
    resolver.AddQuantize(); \
    resolver.AddDequantize(); \
    resolver.AddAveragePool2D(); \
    resolver.AddMaxPool2D(); \
    resolver.AddRelu(); \
    resolver.AddRelu6(); \
    resolver.AddLeakyRelu(); \
    resolver.AddConcatenation(); \
    resolver.AddConv2D(); \
    resolver.AddTransposeConv(); \
    resolver.AddResizeBilinear(); \
    resolver.AddSlice(); \
    resolver.AddStridedSlice(); \
    resolver.AddSplit(); \
    resolver.AddMean();
