# FatigueGuard 系统完整工作流程

> 文档版本: v1.0 · 2026-06-12
> 硬件平台: ESP32-S3N32R16 (16MB PSRAM, 8MB Flash)
> 摄像头: CAM12-720P-V2 (USB UVC, MJPEG, 1280×720@15fps → 处理 5fps)
> 扬声器: 摄像头集成 (USB UAC), 8000Hz/16bit/mono
> 可选外设: TFT 显示屏, EG800K (GPS+4G), MQ-3 酒精传感器

---

## 一、系统架构总览

```
┌────────────────────────────────────────────────────────────────────────────┐
│                        FatigueGuard 系统框图                                 │
│                                                                            │
│  ┌──────────┐    ┌───────────┐    ┌────────────┐    ┌──────────────────┐  │
│  │ USB UVC  │───▶│ esp_jpeg  │───▶│ Face Detect│───▶│   Face Recognizer │  │
│  │ Camera   │    │ Decoder   │    │ (ESP-DL)   │    │  (MobileFaceNet)  │  │
│  │1280×720  │    │RGB565 BE  │    │MSRMNP_S8_V1│    │  112×112→128dim   │  │
│  │ MJPEG    │    │           │    │            │    │  only at startup  │  │
│  └──────────┘    └───────────┘    └─────┬──────┘    └────────┬─────────┘  │
│                                         │                     │            │
│                                         ▼                     ▼            │
│                                  ┌────────────┐       ┌──────────────┐    │
│                                  │  Eye ROI   │       │  NVS (flash) │    │
│                                  │  crop +     │       │  face_db     │    │
│                                  │  →96×96gray │       │  storage     │    │
│                                  └──────┬─────┘       └──────────────┘    │
│                                         ▼                                  │
│                                  ┌────────────┐                           │
│                                  │Eye Classifier│                          │
│                                  │ TFLite Micro │                          │
│                                  │ open/closed  │                          │
│                                  └──────┬─────┘                           │
│                                         ▼                                  │
│                                  ┌────────────┐                           │
│                                  │ PERCLOS    │─── 核心疲劳状态机 ──▶      │
│                                  │窗口30帧~150 │                           │
│                                  │ N/M/S等级  │                           │
│                                  └──────┬─────┘                           │
│                                         │                                  │
│            ┌────────────────────────────┼──────────────────────┐          │
│            ▼                            ▼                      ▼          │
│    ┌───────────┐              ┌──────────────┐        ┌─────────────┐    │
│    │ TFT Display │             │ Voice Prompter│       │ EG800K      │    │
│    │ (SPI/I80)   │             │ (USB UAC)     │       │ GPS+4G      │    │
│    │ ·疲劳等级    │             │ ·语音播报警告  │       │ (UART)      │    │
│    │ ·PERCLOS曲线 │             │ ·疲劳提醒      │       │ ·定位上传    │    │
│    │ ·驾驶员信息   │             │              │       │ ·4G远程通知  │    │
│    │ ·驾驶统计    │             └──────────────┘       │             │    │
│    └───────────┘                                      └─────────────┘    │
│                                                                │          │
│                                                         ┌───────┴─────┐  │
│                                                         │  MQ-3       │  │
│                                                         │ 酒精传感器    │  │
│                                                         │ (ADC/GPIO)  │  │
│                                                         └─────────────┘  │
└──────────────────────────────────────────────────────────────────────────┘

通讯接口:
  ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐
  │ USB-OTG  │   │  SPI     │   │  UART    │   │  I2C/ADC │   │  USB-OTG │
  │ GPIO19/20│   │(Display) │   │(GPS+4G)  │   │ (MQ-3)   │   │ (UAC音   │
  │ (UVC)    │   │          │   │          │   │          │   │  频)     │
  └──────────┘   └──────────┘   └──────────┘   └──────────┘   └──────────┘
```

---

## 二、完整工作流程（按时间线）

### 阶段 0：系统启动

```
  [Power On / Reset]
        │
        ▼
  ┌─────────────────┐
  │ ESP-IDF Boot     │
  │ Mount partitions │
  └─────────┬───────┘
            ▼
  ┌─────────────────┐
  │ app_main()      │
  │ ① 初始化 USB     │ ← usb_host_install()
  │ ② 安装 UVC 驱动  │ ← uvc_host_install()
  │ ③ 初始化眼部分类器│ ← eye_classifier_init()
  │ ④ 初始化人脸识别器│ ← face_recognizer_init() + 加载 NVS 库
  │ ⑤ 加载人脸检测模型│ ← HumanFaceDetect(MSRMNP_S8_V1)
  │ ⑥ 启动帧处理任务  │ ← xTaskCreate(frame_handling_task)
  │ ⑦ 初始化 TFT 屏  │ ← (待实现)
  │ ⑧ 播放开机音效   │ ← (可选) voice_prompter_play(boot.wav)
  │ ⑨ 初始化 GPS+4G  │ ← (待实现) EG800K UART 初始化
  │ ⑩ 初始化 MQ-3   │ ← (待实现) ADC 初始化
  └─────────┬───────┘
            ▼
  ┌─────────────────┐
  │ 显示欢迎页面     │
  │ "FatigueGuard"  │
  │ "等待摄像头..."  │
  └─────────────────┘
```

### 阶段 1：摄像头连接 & 视频流

```
  ┌──────────────────────────────────────────────────────────────┐
  │ frame_handling_task (FreeRTOS task, pinned to core)          │
  │                                                              │
  │  loop:                                                       │
  │   ├── uvc_host_stream_open()  ← 等待 USB 摄像头连接           │
  │   ├── uvc_host_stream_start() ← 开始 MJPEG 流 (1280×720)     │
  │   └── while (s_dev_connected):                               │
  │        │                                                     │
  │        ▼                                                     │
  │  ┌─────────────┐   每 10 帧处理 1 帧 (PROCESS_EVERY_N_FRAMES)│
  │  │ 取帧 xQueue  │ ← frame_callback() 回调入队                │
  │  └──────┬──────┘   丢弃过期帧保持实时性                       │
  │         ▼                                                    │
  │  ┌─────────────┐                                             │
  │  │ MJPEG → RGB565 解码                                      │
  │  │ (esp_jpeg)  │                                             │
  │  └──────┬──────┘                                             │
  │         ▼                                                    │
  │         ↓ (进入 process_frame() 管道)                         │
  └──────────────────────────────────────────────────────────────┘
```

### 阶段 2：人脸检测 & 识别（仅首次）

```
  process_frame():
        │
        ▼
  ┌──────────────────────┐
  │ HumanFaceDetect.run() │ ← 输入 RGB565 全帧
  │ 人脸检测              │    模型: MSRMNP_S8_V1
  └──────────┬───────────┘    阈值: score ≥ 0.1
             │
      ┌──────┴──────┐
      │  检测到人脸?  │──No──▶ 返回 false, 继续下一帧
      └──────┬──────┘
             │ Yes
             ▼
      ┌──────────────────┐
      │ 人脸框尺寸检查     │──<80px──▶ 跳过（人脸太小）
      │ fw≥80 && fh≥80   │
      └────────┬─────────┘
               │
               ▼
      ┌──────────────────┐
      │ 估算眼睛 ROI      │ ← estimate_eye_roi()
      │ 裁剪+双线性缩放   │    参数:
      │ → 96×96 灰度图    │    top=0.22, bottom=0.48
      └────────┬─────────┘    left=0.05, right=0.60
               │
               ▼
      ┌─────────────────────────────────────┐
      │ 首次检测到人脸？(s_face_recognized)   │
      │ 且 非采集模式 且 人脸识别可用          │
      └────────┬────────────────────────────┘
               │ Yes (仅一次)
               ▼
      ┌──────────────────────────┐
      │ FaceRecognizer.extract()  │ ← 从 RGB565 裁剪人脸部位置
      │ 112×112→128维特征向量    │    仿射变换对齐
      └───────────┬──────────────┘
                  │
                  ▼
      ┌──────────────────────────┐
      │ FaceRecognizer.identify() │ ← NVS 中搜索最相似
      │ 与所有注册驾驶员比对      │    余弦相似度阈值 0.30
      └───────────┬──────────────┘
                  │
          ┌───────┴────────┐
          ▼                ▼
    ┌────────────┐  ┌──────────────────────┐
    │ 已知驾驶员   │  │ 陌生人                │
    │ 显示欢迎信息  │  │ 且无人注册 → 自动注册  │
    │ DRIVER #N   │  │ 为 DRIVER #1          │
    │ sim=xx.x%  │  │                      │
    └────────────┘  └──────────────────────┘
         │                 │
         └──────┬──────────┘
                ▼
      ┌──────────────────┐
      │ 发送到 TFT 显示    │ ← 驾驶员身份显示在顶栏
      │ voice_prompter    │ ← face_auth.wav 播报身份
      └──────────────────┘
      ※ 人脸识别只执行一次，后续帧不再触发
```

### 阶段 3：眼部分类 & PERCLOS 计算（循环流水线）

```
                ▼
      ┌──────────────────────┐
      │ eye_classifier_predict│ ← 输入 96×96 灰度图
      │ (TFLite Micro)        │    输出 eye_open, eye_closed
      │ ~50-100ms reference   │
      └───────────┬──────────┘
                  │
                  ▼
          ┌───────┴───────┐
          │ eye_closed >   │──Yes──▶ is_closed=true
          │ eye_open?      │
          └───────┬───────┘
                  │ No
                  ▼
           is_closed=false
                  │
                  ▼
      ┌──────────────────────────┐
      │ PerclosCalculator.add_sample(is_closed) │
      │ 滑动窗口: 30 帧 (默认)    │
      └───────────┬──────────────┘
                  │
                  ▼
      ┌──────────────────────────┐
      │ get_perclos()            │ ← closed_count / window_count × 100
      │ get_fatigue_level()      │
      │  ≤15%  NORMAL            │
      │  >15%  MILD              │
      │  >30%  SEVERE            │
      └───────────┬──────────────┘
                  │
                  ▼
          ┌───────────────┐
          │ FatigueLogger   │ ← 每 30 帧或等级变化时
          │ .update()       │    输出日志到串口
          └───────┬───────┘
                  │
         ┌────────┴────────┐
         ▼                 ▼
  ┌────────────┐  ┌────────────────┐
  │ 等级变化?    │  │ 等级未变        │
  │ 且 >30帧预热 │  │ 继续下一帧      │
  └──────┬─────┘  └────────────────┘
         │ Yes
         ▼
  ┌────────────────────────────────┐
  │ 调用等级变化回调                 │
  │ (level_change_cb_t)            │
  │                                │
  │ 根据新等级触发:                  │
  │  NORMAL → 无动作               │
  │  MILD   → 轻度疲劳提示          │
  │  SEVERE → 严重疲劳警告          │
  │          + 语音播报            │
  │          + 4G 远程通知         │
  │          + TFT 告警显示         │
  └────────────────────────────────┘
```

### 阶段 4：TFT 显示屏更新

```
  每次 frame_handling_task 循环迭代:
        │
        ▼
  ┌──────────────────────────────────────────────┐
  │ 打包显示数据 (结构体)                          │
  │ {                                             │
  │   driver_id,          // 驾驶员 ID             │
  │   fatigue_level,      // NORMAL/MILD/SEVERE   │
  │   perclos_pct,        // 当前 PERCLOS 百分比   │
  │   perclos_history[],  // 最近 N 帧历史          │
  │   total_frames,       // 累计检测帧数           │
  │   closed_frames,      // 累计闭眼帧数           │
  │   drive_time_min,     // 驾驶时长(分钟)         │
  │ }                                             │
  └──────────────────────┬───────────────────────┘
                         │
                         ▼
  ┌──────────────────────────────────────────────┐
  │ TFT Display 渲染 (建议 LVGL 或直接 framebuffer)│
  │                                               │
  │ ① 顶部状态栏:                                  │
  │   DRIVER #1  |  ⏱ 00:12:30                    │
  │   [🟢/🟡/🔴] 疲劳等级指示灯                    │
  │                                               │
  │ ② 中央大字: 疲劳等级                           │
  │   ● NORMAL  (绿色, 正常)                       │
  │   ● MILD    (黄色闪烁, 建议休息)               │
  │   ● SEVERE  (红色闪烁, 严重警告)               │
  │   PERCLOS: 8.5%                               │
  │                                               │
  │ ③ PERCLOS 滚动曲线                             │
  │   ┌────────────────┐                          │
  │   │  ▁▃▆█▇▆▅▄▃▂▁..  │  ← 最近 60 帧趋势      │
  │   └────────────────┘                          │
  │   15% ─ ─ ─ ─ ─ ─ ─     30% ─ ─ ─ ─ ─ ─ ─  │
  │                                               │
  │ ④ 底部辅助信息                                 │
  │   👁 OPEN: 42  |  🔒 CLOSED: 8 (16%)          │
  │   🚗 已行驶 12 分钟  |  🔊 语音播报中           │
  └──────────────────────────────────────────────┘
```

### 阶段 5：语音播报

```
  触发条件:
  ┌──────────────────────────────────────────┐
  │ 等级变化 → 语音播报                       │
  │                                          │
  │ FatigueLevel::MILD                       │
  │   → voice_prompter_play(mild.wav)        │
  │     "轻度疲劳，建议您休息一下"             │
  │                                          │
  │ FatigueLevel::SEVERE                     │
  │   → voice_prompter_play(severe.wav)      │
  │     "严重疲劳！请立即停车休息！"           │
  │                                          │
  │ 首次人脸识别成功                           │
  │   → voice_prompter_play(face_auth.wav)   │
  │     "驾驶员已验证"                         │
  │                                          │
  │ 超长驾驶 (≥2小时)                          │
  │   → voice_prompter_play(drive_long.wav)  │
  │     "您已长时间驾驶，请注意休息"           │
  └──────────────────────────────────────────┘

  技术限制:
  ┌──────────────────────────────────────────┐
  │ ⚠ USB 带宽冲突: UAC + UVC 共用            │
  │   USB-OTG (GPIO19/20) 导致 MJPEG 断流     │
  │   当前: UAC 已禁用 (main.cpp:489)         │
  │   解决方案: 改用独立 I2S DAC 扬声器         │
  │   (如 MAX98357 + I2S) 或 PCM5102         │
  └──────────────────────────────────────────┘
```

### 阶段 6：GPS + 4G 远程通知（可选外设 EG800K）

```
  ┌─────────────────────────────────────────────────────────────┐
  │ EG800K 模块 (UART 通讯)                                      │
  │                                                             │
  │ ① 初始化: AT 指令配置网络                                   │
  │    AT+CGATT=1       → 附着 GPRS 网络                       │
  │    AT+CGPS=1        → 开启 GPS                             │
  │                                                             │
  │ ② 定时获取位置 (每 60 秒)                                   │
  │    AT+CGPSINFO      → 解析经纬度/速度/方向                   │
  │    → 存储到全局: s_gps_lat, s_gps_lon, s_gps_speed          │
  │                                                             │
  │ ③ 疲劳事件 → 远程通知                                       │
  │    ┌─────────────────────────────────────┐                  │
  │    │ 当 FatigueLevel::SEVERE 触发时:      │                  │
  │    │                                     │                  │
  │    │ 1. 打包告警数据:                     │                  │
  │    │    {                                 │                  │
  │    │      event: "FATIGUE_SEVERE",        │                  │
  │    │      time: "2026-06-12 14:30:00",    │                  │
  │    │      location: "22.5431°N, 114.0579°E",│                │
  │    │      speed: "65 km/h",               │                  │
  │    │      perclos: "42.5%",               │                  │
  │    │      driver: "DRIVER #1"             │                  │
  │    │    }                                 │                  │
  │    │                                     │                  │
  │    │ 2. 通过 4G 发送到服务器:              │                  │
  │    │    AT+HTTPINIT                       │                  │
  │    │    AT+HTTPURL="https://api.xxx.com/  │                  │
  │    │                  fatigue/alert"      │                  │
  │    │    AT+HTTPPOST=POST, JSON 数据       │                  │
  │    │                                     │                  │
  │    │ 3. 发送短信给紧急联系人:               │                  │
  │    │    AT+CMGS="+8613800000000"          │                  │
  │    │    "疲劳告警: 驾驶员#1 严重疲劳,       │                  │
  │    │     位置: xx, 速度: 65km/h"          │                  │
  │    └─────────────────────────────────────┘                  │
  │                                                             │
  │ ④ 定时上报位置 (每 5 分钟)                                  │
  │    即使无疲劳事件，也定时上报车辆位置                        │
  └─────────────────────────────────────────────────────────────┘
```

### 阶段 7：MQ-3 酒精检测（可选）

```
  ┌─────────────────────────────────────────┐
  │ MQ-3 酒精传感器 (ADC1_CHx / GPIO)        │
  │                                         │
  │ ① 系统启动时预热 60 秒                    │
  │ ② 每 10 秒采样一次 ADC                   │
  │ ③ 将 ADC 值转换为酒精浓度 (PPM)          │
  │ ④ 阈值判断:                              │
  │    < 0.05 mg/L → 正常, 允许驾驶          │
  │    ≥ 0.05 mg/L → 警告, 显示"酒精超标"    │
  │                                         │
  │ ⑤ 超标时:                                │
  │    · 显示屏显示酒精警告                    │
  │    · 语音播报"酒精检测超标"               │
  │    · 通过 4G 发送告警通知                 │
  └─────────────────────────────────────────┘
```

---

## 三、任务调度 & 时序

```
FreeRTOS 任务分布（ESP32-S3 双核）：

┌──────────────────────────────────────────────────────────────────┐
│ Core 0 (Protocol)                       │ Core 1 (Application)   │
├──────────────────────────────────────────────────────────────────┤
│                                      │                          │
│  Task: usb_lib (prio 15)              │  Task: uvc_frame        │
│  ┌─────────────────────┐              │  (prio 13, stack 16KB)  │
│  │ usb_host_handle_    │              │  ┌───────────────────┐  │
│  │ events() loop       │              │  │ 取MJPEG→JPEG解码  │  │
│  └─────────────────────┘              │  │ →人脸检测          │  │
│                                        │  │ →ROI裁剪           │  │
│  Task: UVC Driver (prio 16)           │  │ →眼部分类          │  │
│  ┌─────────────────────┐              │  │ →PERCLOS           │  │
│  │ USB ISO 传输处理     │              │  │ →TFT显示          │  │
│  │ →frame_callback     │              │  └───────────────────┘  │
│  └─────────────────────┘              │                          │
│                                        │  Task: TFT刷新          │
│  (Task: UAC Audio) - 待实现            │  (可选独立任务)          │
│  ┌─────────────────────┐              │  ┌───────────────────┐  │
│  │ USB Audio 播放任务   │              │  │ 从队列取显示数据    │  │
│  └─────────────────────┘              │  │ SPI DMA传输        │  │
│                                        │  └───────────────────┘  │
│   Task: EG800K (4G+GPS)                 │                          │
│   ┌─────────────────────┐              │                          │
│   │ UART AT 指令通讯     │              │                          │
│   │ 定时上报位置/告警    │              │                          │
│   └─────────────────────┘              │                          │
└──────────────────────────────────────────────────────────────────┘

关键时序指标:

┌──────────────┬────────────┬──────────────┬─────────────────────┐
│ 操作          │ 耗时        │ 频率          │ 说明                 │
├──────────────┼────────────┼──────────────┼─────────────────────┤
│ MJPEG 解码    │ ~30-50ms   │ 每 10 帧 1 次  │ esp_jpeg 640×480→RGB │
│ 人脸检测      │ ~50-100ms  │ 每 10 帧 1 次  │ MSRMNP_S8_V1         │
│ 人脸识别      │ ~12.6 秒   │ 仅启动时 1 次  │ MobileFaceNet 推理    │
│ 眼睛 ROI 裁剪  │ <1ms      │ 每 10 帧 1 次  │ 定点数双线性缩放      │
│ 眼部分类推理   │ ~50-100ms  │ 每 10 帧 1 次  │ TFLite reference ops  │
│ PERCLOS 计算  │ <1ms      │ 每 10 帧 1 次  │ O(1) 滑动窗口         │
│ TFT 刷新      │ ~5-20ms   │ 每 10 帧 1 次  │ SPI 或 I80 接口       │
│ GPS 位置获取  │ ~1-3 秒   │ 每 60 秒 1 次  │ AT+CGPSINFO           │
│ 4G 远程上报   │ ~1-5 秒   │ 告警/定时       │ HTTP POST / SMS      │
│ MQ-3 采样     │ <1ms      │ 每 10 秒 1 次  │ ADC 读取              │
└──────────────┴────────────┴──────────────┴─────────────────────┘

处理管道总延时 (每帧): ~130-250ms
有效帧率: 1280×720@15fps 输入 → 每 10 帧处理 1 帧 → ~1.5 fps 输出
```

---

## 四、数据流图

```
┌──────────┐    JPEG      ┌──────────┐   RGB565     ┌───────────┐
│ USB UVC  │─────────────▶│ esp_jpeg │─────────────▶│ Face Detect│
│ 1280×720 │  MJPEG 帧    │ Decoder  │  全帧 RGB565 │ (ESP-DL)  │
│ 15fps    │              │          │              └─────┬─────┘
└──────────┘              └──────────┘                    │
                                                    ┌─────┴─────┐
                                                    │ 人脸框坐标  │
                                                    │ [x1,y1,x2,y2]│
                                                    └─────┬─────┘
                                                          │
                 ┌────────────────────────────────────────┘
                 │
                 ▼
        ┌────────────────┐  灰度96×96   ┌─────────────────┐  open/closed
        │ Eye ROI crop   │────────────▶│ Eye Classifier   │────────────▶
        │ + bilinear     │   uint8[9216]│ TFLite Micro     │
        │ + gray convert  │              │ (INT8 quantized)  │
        └────────────────┘              └─────────────────┘
                                            │ 闭眼判断
                                            ▼
        ┌─────────────────────────────────────────┐
        │ PerclosCalculator<30> (环形缓冲 bool[30])│
        │   add_sample(is_closed)                  │
        │   → PERCLOS% = closed_count / count × 100│
        │   → FatigueLevel: NORMAL/MILD/SEVERE     │
        └────────────────┬────────────────────────┘
                         │
                         ▼
              ┌─────────────────────┐
              │ 输出到各外设          │
              ├─────────────────────┤
              │ → TFT Display       │ ← 曲线 + 等级
              │ → Voice (UAC/I2S)   │ ← 疲劳语音提醒
              │ → 4G (EG800K)       │ ← 远程告警
              │ → Serial Monitor    │ ← 调试日志
              │ → NVS (face_db)     │ ← 驾驶员特征存档
              └─────────────────────┘
```

---

## 五、状态机：疲劳等级转换

```
                  ┌─────────┐
        ┌───────▶│ NORMAL  │◀────────┐
        │        │ ≤15%    │         │
        │        └────┬────┘         │
        │             │              │
        │      PERCLOS > 15%         │ PERCLOS ≤ 15%
        │             │              │
        │             ▼              │
        │        ┌─────────┐         │
        │        │  MILD   │─────────┘
        │        │ 15-30%  │
        │        └────┬────┘
        │             │
        │      PERCLOS > 30%
        │             │
        │             ▼
        │        ┌──────────┐
        │        │ SEVERE   │─────────┐
        │        │ >30%     │         │
        │        └──────────┘         │
        │                             │
        └─────────────────────────────┘ PERCLOS ≤ 15%

等级转换触发动作:

  NORMAL → MILD:
     • TFT: 等级指示从绿变黄
     • TFT: 显示"建议休息"文字
     • 语音: mild.wav (轻度疲劳建议)
     • 记录: 累加一次轻度事件

  MILD → SEVERE:
     • TFT: 等级指示从黄变红, 闪烁
     • TFT: PERCLOS 曲线高亮红色区域
     • 语音: severe.wav (严重疲劳警告)
     • 4G:  发送告警到服务器 + 短信通知
     • GPS:  附带位置信息

  SEVERE → MILD / NORMAL:
     • TFT: 恢复黄色/绿色
     • 停止闪烁
     • 4G:  可选发送"已恢复"通知

  同等级维持:
     • TFT: 每 30 帧刷新一次曲线
     • 无语音/4G 动作
```

---

## 六、接口定义（模块间通讯）

### 6.1 核心数据结构

```cpp
// main.cpp 内部全局状态 (用于模块间传递)
struct fatigue_display_data_t {
    int         driver_id;          // -1=未知, 1..N=已知驾驶员
    char        driver_name[16];    // 驾驶员名称
    float       confidence;         // 人脸识别置信度 (0~1)
    
    FatigueLevel level;             // 当前疲劳等级
    float       perclos;            // 当前 PERCLOS 百分比 (%)
    float       perclos_history[60]; // 最近 60 帧 PERCLOS 历史
    int         history_len;        // 历史长度
    
    uint32_t    total_frames;       // 累计检测帧数
    uint32_t    closed_frames;      // 累计闭眼帧数
    uint32_t    face_frames;        // 累计检测到人脸的帧数
    uint32_t    drive_time_sec;     // 本次驾驶时长 (秒)
    
    // GPS (可选)
    double      latitude;           // 纬度
    double      longitude;          // 经度
    float       speed_kmh;          // 速度 (km/h)
    
    // MQ-3 (可选)
    float       alcohol_ppm;        // 酒精浓度
    bool        alcohol_over_limit; // 是否超标
    
    // 系统状态
    bool        camera_connected;
    bool        gps_fixed;
    bool        network_connected;
};
```

### 6.2 TFT 显示接口

```cpp
// display.h (建议新建组件)
// 输入: fatigue_display_data_t 结构体
// 内部: 维护 framebuffer, SPI/I80 传输

void display_init(void);                                   // 初始化屏幕
void display_update(const fatigue_display_data_t *data);   // 更新全屏显示

// 低层级:
void display_draw_fatigue_status(FatigueLevel level, float perclos);
void display_draw_perclos_curve(const float history[], int len);
void display_draw_driver_info(int driver_id, const char *name, float conf);
void display_draw_stats(uint32_t total, uint32_t closed, uint32_t drive_sec);
void display_draw_gps(double lat, double lon, float speed);
void display_draw_alcohol(float ppm, bool over_limit);
void display_show_message(const char *msg, uint32_t color); // 临时消息
```

### 6.3 4G/GPS 接口

```cpp
// gps_4g.h (建议新建组件)
void gps_4g_init(uart_port_t uart_num, int tx_pin, int rx_pin);
bool gps_4g_get_position(double *lat, double *lon, float *speed);
bool gps_4g_send_alert(const char *json_data);
bool gps_4g_send_sms(const char *phone, const char *message);
void gps_4g_deinit(void);
```

### 6.4 MQ-3 接口

```cpp
// mq3.h (建议新建组件)
void mq3_init(adc_channel_t channel);    // 预热 60 秒
float mq3_read_ppm(void);                // 读酒精浓度 (mg/L)
bool mq3_is_over_limit(void);            // 是否超标 (>0.05 mg/L)
```

---

## 七、当前进度 & 待实现清单

```
  阶段       | 状态     | 模块         | 备注
─────────────┼──────────┼──────────────┼──────────────────────────
  数据采集    │ ✅ 待采集 │ 真实摄像头数据 │ 硬件已就绪, 等待采集标注
  ───────────┼──────────┼──────────────┼──────────────────────────
  阶段一      │ ✅ 完成  │ 人脸检测      │ HumanFaceDetect MSRMNP_S8_V1
  阶段二      │ ✅ 完成  │ 眼睛 ROI 裁剪 │ 最终参数已定稿
  阶段三-A    │ ✅ 完成  │ USB UVC 摄像头│ CAM12-720P-V2, 1280×720
  阶段三-B    │ ✅ 完成  │ 眼部分类+PERCLOS│ TFLite reference ops
  阶段三-C    │ ⏳ 待办  │ 重新训练模型  │ 需采集数据→Edge Impulse→导出
  阶段三-D    │ ✅ 完成  │ 人脸识别      │ MobileFaceNet + NVS
  ───────────┼──────────┼──────────────┼──────────────────────────
  阶段四-A    │ ⏳ 待办  │ TFT 显示屏    │ 你的队友正在开发曲线绘制
  阶段四-B    │ ⛔ 阻塞  │ 语音播报(UAC) │ USB 带宽冲突, 需改 I2S DAC
  阶段四-C    │ ⏳ 待办  │ 开机/身份播报 │ 依赖语音硬件方案
  ───────────┼──────────┼──────────────┼──────────────────────────
  阶段五-A    │ ⏳ 待办  │ EG800K 4G    │ 可选外设, UART 通讯
  阶段五-B    │ ⏳ 待办  │ EG800K GPS   │ 可选外设, AT+CGPSINFO
  阶段五-C    │ ⏳ 待办  │ 远程告警      │ 疲劳严重时 HTTP POST + SMS
  ───────────┼──────────┼──────────────┼──────────────────────────
  阶段五-D    │ ⏳ 待办  │ MQ-3 酒精检测 │ 可选外设, ADC 采样
```

---

## 八、关键限制 & 决策记录

| 限制 | 说明 | 当前方案 / 替代方案 |
|------|------|---------------------|
| **USB 带宽冲突** | UAC + UVC 共用 USB-OTG, 同时使用导致 MJPEG 断流 | 语音改为 **I2S DAC** (MAX98357/PCM5102), 释放 USB 带宽 |
| **PSRAM vs DMA** | Tensor arena 放 PSRAM 会被 USB DMA 损坏 | arena 放内部 DRAM (`MALLOC_CAP_DMA \| MALLOC_CAP_INTERNAL`) |
| **人脸识别慢** | MobileFaceNet 推理 ~12.6 秒 (reference ops) | 只在启动时首次检测到人脸执行一次 |
| **眼部分类泛化** | 当前模型用网络图片训练, 不匹配真实摄像头管道 | 待办: 采集真实数据→Edge Impulse 重新训练 |
| **ESP-NN INT8 饱和** | 全部 INT8 算子有溢出 bug | 已退回到 reference ops |
| **板载 USB 失效** | USB-OTG 占用 PHY, USB-Serial-JTAG 不可用 | 必须用外部 UART (GPIO43/44) 看日志 |
| **Flash 分区** | 3.5MB app + 剩余存储 | TFLite 模型和人脸模型已占大量空间 |
