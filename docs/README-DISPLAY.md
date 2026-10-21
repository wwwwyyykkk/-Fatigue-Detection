# 🖥️ TFT 显示屏驱动 — 工作方向与接口说明

**组员角色：** 显示屏驱动开发  
**主要任务：** TFT 显示屏驱动 + PERCLOS 疲劳曲线实时显示  
**目标芯片：** ESP32-S3N32R16（16MB Octal PSRAM）  
**主项目位置：** `Project/`（ESP-IDF v5.5.4 项目）

---

## ⚠️ 前置准备：先把项目编译跑起来

拿到代码包后不要直接开始写 TFT 驱动，**先按以下步骤确保项目能编译烧录**，再进行开发。

### ① 安装 ESP-IDF v5.5.4

如果还没装，去官网安装 ESP-IDF v5.5.4：

> **离线安装（推荐，约 1.2GB）：**
> 1. 下载 [ESP-IDF v5.5.4 离线安装器](https://dl.espressif.com/dl/esp-idf/)
> 2. 安装时勾选 **ESP32-S3** 支持
> 3. 安装完成后会在开始菜单生成 `ESP-IDF 5.5 CMD` / `ESP-IDF 5.5 PowerShell`

**注意：** 版本号尽量用 v5.5.x，不要用 v4.x 或 v5.0~v5.3，项目配置基于 v5.5 的驱动 API。

### ② 修改 `activate.ps1` 中的 ESP-IDF 路径

原文件内容：
```powershell
# 文件位置：Project/activate.ps1
. D:\ESP\.espressif\v5.5.4\esp-idf\export.ps1
$env:PYTHONIOENCODING = "utf-8"
```

**把你电脑上实际的 ESP-IDF 安装路径替换进去。** 常见路径：

| 安装方式 | export.ps1 的路径 |
|---------|------------------|
| 在线安装器 | `C:\Espressif\frameworks\esp-idf-v5.5.4\export.ps1` |
| 离线安装器 | `C:\Espressif\frameworks\esp-idf-v5.5.4\export.ps1` |
| Git clone | 你 clone 的路径下的 `export.ps1` |
| ESP-IDF 容器 | 容器内对应路径 |

> 如果找不到 `export.ps1`，在文件管理器搜索一下即可。

### ③ 编译方法

```powershell
cd 你的路径\Project
.\activate.ps1
idf.py build
```

**⚠️ 首次编译注意事项：**
- **首次编译很慢**（约 2 分钟），因为 TFLite Micro 有大量 C++ 模板实例化
- cmake 配置阶段需要**联网检查组件管理器依赖**（虽然代码包里已包含，但 cmake 仍会尝试联网校验）
  - 国内网络环境不稳时可能卡在 `Running cmake` 或 `edit-manifest` 阶段 70~120 秒
  - **解决方法：** 耐心等它超时或跳过，只要 `managed_components/` 目录在就不会重新下载
  - 如果永久卡死 → 按 `Ctrl+C` 杀掉进程，检查网络，重试
- 以后增量编译就快了（除非删了 `build/` 目录）

### ④ 烧录指南

用 USB 线连接 ESP32-S3 开发板到电脑，**摄像头不需要拔**（杜邦线不影响烧录）。

```powershell
cd 你的路径\Project
.\activate.ps1

# 先查一下你的串口号（设备管理器中查看）
# 假设是 COM3：
idf.py -p COM3 flash monitor

# 退出 monitor 按 Ctrl + ]
```

**⚠️ 注意：**
- `COM16` 是我电脑上的串口号，**你必须改成你自己的**（通常是 COM3 或 COM4）
- 烧录时 ESP32-S3 的板载 USB-Serial-JTAG 接口会临时接管，烧完后 monitor 看日志
- 如果 `idf.py flash` 卡住 → 用 esptool 直烧（见下方备选方案）

### ⑤ 没摄像头也能调试 TFT 显示

你手头可能没有 USB UVC 摄像头（CAM12-720P-V2）。**开发 TFT 界面不需要摄像头**，你可以这样做：

**方案 A：手动构造模拟数据**
```cpp
// 在 main.cpp 的 app_main() 末尾，先不初始化摄像头：
while (1) {
    // 模拟 PERCLOS 数据（正弦波 0%~50% 来回变化）
    float fake_perclos = 25.0f + 25.0f * sin(x * 0.1f);
    
    // 填充显示数据结构 → 触发 TFT 刷新
    tft_display_update(...);  // 你的函数
    
    vTaskDelay(pdMS_TO_TICKS(1000));  // 每秒刷一次
    x++;
}
```

**方案 B：注释掉摄像头初始化，用纯模拟模式**
在 `app_main()` 中注释掉 `frame_handling_task` 的创建，启动一个模拟任务专门喂 PERCLOS 数据给 TFT。这样根本不需要摄像头硬件就能调试显示界面。

> **建议流程：** 先方案 B 搞模拟模式调好 UI → 再连真实摄像头联调完整流水线。

### ⑥ 备选：esptool 直烧（当 `idf.py flash` 卡死时）

```powershell
cd 你的路径\Project\build
.\..\activate.ps1
python $env:IDF_PATH\components\esptool_py\esptool\esptool.py --chip esp32s3 -p COM3 -b 460800 --before=default_reset --after=hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB 0x0 bootloader\bootloader.bin 0x8000 partition_table\partition-table.bin 0x10000 fatigue_detection.bin
```

（这里串口也要改成你实际的端口号）

### ⑦ 调试串口日志

烧录后看日志需要外部 UART（板载 USB 口在 UVC 模式下会失效）：
- 用 USB 转 TTL 模块接 **GPIO43 (TX)** / **GPIO44 (RX)** / GND
- 波特率 **115200**

如果手头没有 USB 转 TTL，可以先用模拟模式调试 TFT，不接摄像头就不需要看日志。

---

### 前置准备清单（打勾确认）

- [ ] ESP-IDF v5.5.4 已安装
- [ ] `activate.ps1` 路径已改成你的 ESP-IDF 路径
- [ ] `idf.py build` 编译通过
- [ ] 串口号已确认（`-p COMx`）
- [ ] 烧录到板子跑起来（至少能看到串口日志输出）
- [ ] 已决定用模拟数据还是接摄像头调试 TFT

---

## 目录

1. [系统整体流水线](#1-系统整体流水线)
2. [你的工作范围](#2-你的工作范围)
3. [需要对接的接口（重点）](#3-需要对接的接口重点)
4. [建议的显示内容](#4-建议的显示内容)
5. [硬件接线参考](#5-硬件接线参考)
6. [TFT 显示屏选项](#6-tft-显示屏选项)
7. [组件结构建议](#7-组件结构建议)
8. [集成步骤](#8-集成步骤)
9. [交付物清单](#9-交付物清单)
10. [常见坑与注意事项](#10-常见坑与注意事项)
11. [附录：数据流图](#11-附录数据流图)

---

## 1. 系统整体流水线

```
USB UVC 摄像头 (1280×720 MJPEG)
    │
    ▼ 每10帧处理1帧（跳过9帧）
JPEG 解码 → RGB565
    │
    ▼
人脸检测 (ESP-DL HumanFaceDetect)
    │
    ▼ 找到人脸
眼睛 ROI 裁剪 (eye_roi.hpp) → 96×96 灰度图
    │
    ├──→ 首次人脸？→ 人脸识别 (MobileFaceNet) ──→ NVS 存储
    │                              └──→ 播放 face_detect.wav
    │
    ▼
眼部分类器 (MobileNetV2 0.35 INT8) → eye_open / eye_closed
    │
    ▼
PERCLOS 计算器 (30帧滑动窗口 ≈6秒)
    │
    ▼
疲劳等级判定 (NORMAL / MILD / SEVERE)
    │
    ├──→ ESP_LOGI 日志输出
    ├──→ level_change_cb → 语音播报 (voice_prompter)
    │
    └──→ ⭐ **你的任务：SPI TFT 显示 PERCLOS 曲线 + 状态面板**
```

### 关键时序

| 阶段 | 耗时 | 频率 |
|------|------|------|
| JPEG 解码 + 人脸检测 | ~xx ms | 每 10 帧（≈2次/秒 @ 15fps） |
| 眼部分类推理 | ~xx ms | 每 10 帧（有人脸时） |
| 人脸识别 | ~12.6 秒 | 仅启动时一次 |
| **TFT 刷新** | **待你实现** | **建议每 2~5 秒刷新一次** |

---

## 2. 你的工作范围

### 必需功能（优先级高）

1. **TFT 显示屏驱动（SPI 接口）**
   - 初始化 LCD（使用 ESP-IDF `spi_master` 驱动）
   - 实现画点、画矩形、填充、显示中英文字符等基础图形库
   - 建议基于 **LVGL（LittlevGL）** 或 **ESP-IDF LCD 驱动框架**

2. **PERCLOS 实时曲线显示**
   - 从 `PerclosCalculator` 读取当前 PERCLOS 百分比
   - 在屏幕上绘制滚动波形图（X 轴=时间/帧数，Y 轴=PERCLOS%）
   - 标记阈值线：正常 ≤15% / 轻度 15%~30% / 严重 >30%

3. **疲劳状态面板**
   - 当前等级：正常（绿色）/ 轻度（黄色）/ 严重（红色）
   - 当前 PERCLOS 数值（xx.x%）
   - 窗口填充进度（采集到 x/30 帧）

### 扩展功能（优先级中）

4. **人脸检测状态指示**
   - 是否检测到人脸（/ 丢失人脸）
   - 人脸框大小

5. **驾驶统计信息**
   - 总检测帧数、人脸帧数、闭眼帧数
   - 驾驶时长

6. **驾驶员身份显示**
   - 当前驾驶员 ID
   - 相似度百分比

### 可选功能（优先级低）

7. 主菜单 / 设置界面
8. 亮度调节
9. 报警弹窗（严重疲劳时闪烁）

---

## 3. 需要对接的接口（重点）

### 3.1 PERCLOS 数据源 — `main/perclos.hpp`

这是唯一需要读取的数据结构，所有 PERCLOS 信息都在 `main.cpp` 的 `frame_handling_task` 中计算。

```cpp
// 在 frame_handling_task 中（main.cpp:315-320）
PerclosCalculator<30> s_perclos;          // 30帧滑动窗口
FatigueLogger s_fatigue_log;             // 疲劳等级变化日志
uint32_t total_frames = 0, face_frames = 0, closed_frames = 0;
```

**你需要读取的数据：**

| 数据 | 类型 | 获取方式 | 说明 |
|------|------|---------|------|
| PERCLOS 百分比 | `float` | `s_perclos.get_perclos()` | 0.0~100.0 |
| 疲劳等级 | `FatigueLevel` | `s_perclos.get_fatigue_level()` | NORMAL/MILD/SEVERE |
| 窗口填充数 | `int` | `s_perclos.get_valid_count()` | 0~30 |
| 窗口是否稳定 | `bool` | `s_perclos.is_warm()` | 满30帧=true |
| 总帧数 | `uint32_t` | `total_frames` | 处理过的总帧数 |
| 人脸帧数 | `uint32_t` | `face_frames` | 检测到人脸的帧数 |
| 闭眼帧数 | `uint32_t` | `closed_frames` | 判为闭眼的帧数 |

### 3.2 人脸检测状态 — `main.cpp`

人脸检测结果来自 `process_frame()` 内的 `dl::detect::result_t`：

```cpp
// process_frame 中
const auto &face = results.front();
int fw = face.box[2] - face.box[0];  // 人脸框宽度
int fh = face.box[3] - face.box[1];  // 人脸框高度
float score = face.score;            // 检测置信度
```

### 3.3 集成方式（二选一）

#### 方案 A：全局变量共享（推荐，简单直接）

在 `main.cpp` 中定义全局结构体，TFT 组件直接读取：

```cpp
// 在 main.cpp 中（或独立的 display_data.h）
typedef struct {
    float    perclos;           // 当前 PERCLOS
    bool     face_detected;     // 是否检测到人脸
    int      face_w, face_h;    // 人脸框大小
    FatigueLevel fatigue_level;  // 疲劳等级
    uint32_t total_frames;      // 总帧数
    uint32_t face_frames;       // 人脸帧数
    uint32_t closed_frames;     // 闭眼帧数
    bool     is_warm;           // PERCLOS 窗口填满
    int      valid_count;       // 窗口有效帧数
    int      driver_id;         // 驾驶员 ID（-1=未知）
    float    driver_similarity; // 驾驶员相似度
} display_data_t;

extern display_data_t g_display_data;  // 全局变量，TFT 组件读取
```

然后在 `frame_handling_task` 中每帧更新：

```cpp
g_display_data.perclos = s_perclos.get_perclos();
g_display_data.fatigue_level = s_perclos.get_fatigue_level();
g_display_data.face_detected = face_found;
// ... 其余字段
```

#### 方案 B：回调函数注册（更优雅）

在 `FatigueLogger` 中已有 `level_change_cb_t` 模式，可以扩展：

```cpp
// 在 main.cpp 中注册 TFT 更新回调
tft_display_init();
register_perclos_callback(my_tft_update_cb);
```

**建议采用方案 A**，原因：显示屏刷新不需要严格的实时性（每 2~5 秒刷一次即可），全局变量共享最简单且不影响现有逻辑。

### 3.4 关键代码位置

| 文件 | 内容 |
|------|------|
| `main/main.cpp` | 主流水线，所有关键变量在此 |
| `main/perclos.hpp` | PERCLOS 计算器 + FatigueLogger |
| `main/eye_roi.hpp` | 人脸框→眼睛 ROI 裁剪 |
| `components/eye_classifier/` | 眼部分类器 TFLite Micro |
| `components/face_recognizer/` | 人脸识别 MobileFaceNet |
| `components/voice_prompter/` | I2S 语音播报（参考组件写法） |

---

## 4. 建议的显示内容

### 4.1 布局建议（320×240 或 240×240 屏幕）

```
┌──────────────────────────────┐
│  🔵 疲劳监测   DRIVER #1     │ ← 标题栏 + 驾驶员
│  ┌──────────────────────────┐│
│  │   PERCLOS 实时曲线       ││
│  │  30% ──────────·─       ││ ← 严重阈值线 (30%)
│  │     ·         ·         ││
│  │  15% ──·───·───·─      ││ ← 轻度阈值线 (15%)
│  │   0% · · · · · · · ·    ││ ← 曲线
│  │    ──┬──┬──┬──┬──┬──    ││
│  │    10  20  30  40  50(帧)││
│  └──────────────────────────┘│
│  当前：13.5% ● 轻度疲劳       │ ← 状态行（颜色标记）
│  人脸：✓ 检测中  总帧: 1520   │ ← 统计信息
│  闭眼：45/632 (7.1%)         │
└──────────────────────────────┘
```

### 4.2 颜色方案

| 状态 | 颜色 |
|------|------|
| 正常 (≤15%) | 绿色 `0x07E0` |
| 轻度 (15%~30%) | 黄色 `0xFFE0` |
| 严重 (>30%) | 红色 `0xF800` |
| 背景 | 黑色 `0x0000` |
| 文本 / 网格线 | 白色 / 灰色 |

### 4.3 PERCLOS 历史缓冲区

建议在 TFT 组件内维护一个循环历史缓冲区，用于绘制曲线：

```cpp
#define PERCLOS_HISTORY_SIZE 120  // 显示 120 个历史点

class PerclosDisplayBuffer {
    float buffer[PERCLOS_HISTORY_SIZE];
    int head = 0;
    int count = 0;
    
public:
    void push(float value) { ... }
    float get(int index) { ... }
    int size() { return count; }
};
```

每收到一个新 PERCLOS 值（约每 6 秒 1 个稳定值），push 到缓冲区并重绘曲线。

---

## 5. 硬件接线参考

### SPI TFT 标准接线（以 ST7789 / ILI9341 为例）

| TFT 引脚 | 开发板 GPIO | 说明 |
|---------|------------|------|
| TFT_CS  | GPIO10     | SPI 片选（可配置） |
| TFT_DC  | GPIO11     | 数据/命令选择 |
| TFT_RST | GPIO12     | 复位（可连 EN） |
| TFT_SCLK| GPIO13     | SPI 时钟 |
| TFT_MOSI| GPIO14     | SPI 数据（主出从入） |
| TFT_BL  | GPIO15     | 背光（PWM 可调） |
| VCC     | 3.3V       | 注意：部分 5V 兼容 |
| GND     | GND        | 共地 |

> **⚠️ 注意 GPIO 冲突检查**
> 确保所选 GPIO 不与以下冲突：
> - I2S MAX98357：GPIO18(BCK), GPIO17(LRC), GPIO16(DIN)
> - USB-OTG：GPIO19(D-), GPIO20(D+)
> - UART TX/RX（调试口）：GPIO43(TX), GPIO44(RX)

### 推荐 PIN 方案

```
TFT_CS   = GPIO4
TFT_DC   = GPIO5
TFT_RST  = GPIO6   (或直接接 3.3V EN)
TFT_SCLK = GPIO7   (SPI2 默认 SCK)
TFT_MOSI = GPIO8   (SPI2 默认 MOSI)
TFT_MISO = GPIO9   (不需要读取的话可 NC)
TFT_BL   = GPIO15  (PWM 调速)
```

使用 `SPI2_HOST`（`VSPI`），与 PSRAM / Flash 的 SPI 控制器不冲突。

---

## 6. TFT 显示屏选项

### 推荐型号

| 型号 | 分辨率 | 尺寸 | 驱动 IC | 接口 | 推荐理由 |
|------|--------|------|---------|------|---------|
| **1.3" IPS LCD** | 240×240 | 1.3寸 | ST7789 | SPI | 小、便宜、够用 |
| **1.69" IPS LCD** | 280×280 | 1.69寸 | ST7789V | SPI | 显示面积略大 |
| **2.0" IPS LCD** | 320×240 | 2.0寸 | ILI9341 | SPI | 显示内容多 |
| **2.8" TFT LCD** | 320×240 | 2.8寸 | ILI9341 | SPI | 大屏好操作 |

### LVGL 建议

如果使用 LVGL（推荐），则：
- 显示双缓冲区（`LVGL draw buffer`）放在 PSRAM
- 使用 `disp_driver` + `esp_lcd` 框架
- 参考 ESP-IDF 例程：`esp-idf/examples/peripherals/lcd/tjpgd/`
- 或 `esp-idf/examples/peripherals/lvgl/demos/`

---

## 7. 组件结构建议

在 `Project/components/` 下创建新组件：

```
components/tft_display/
├── CMakeLists.txt              ← REGISTER tft_display 组件
├── include/
│   └── tft_display.h           ← C API 头文件
├── tft_display.cpp             ← 主实现
├── tft_display_lvgl.cpp        ← LVGL 绑定（如果使用）
└── fonts/                      ← 字库文件（可选）
```

### 建议的 C API

```cpp
// tft_display.h
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 显示数据更新结构体（由 main.cpp 填充） */
typedef struct {
    float    perclos;
    uint8_t  fatigue_level;  // 0=normal, 1=mild, 2=severe
    bool     face_detected;
    int      face_w, face_h;
    uint32_t total_frames;
    uint32_t face_frames;
    uint32_t closed_frames;
    int      valid_count;
    int      driver_id;
    float    driver_similarity;
} tft_display_data_t;

/** @brief 初始化 TFT 显示屏 */
bool tft_display_init(void);

/** @brief 更新显示数据（每帧从 PERCLOS 任务调用） */
void tft_display_update(const tft_display_data_t *data);

/** @brief 要求立即刷新屏幕（从 TFT 组件自己的定时器调用） */
void tft_display_request_refresh(void);

/** @brief 设置背光亮度 0-100 */
void tft_display_set_brightness(uint8_t percent);

/** @brief 反初始化 */
void tft_display_deinit(void);

#ifdef __cplusplus
}
#endif
```

### CMakeLists.txt 模板

```cmake
idf_component_register(
    SRCS "tft_display.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        driver
        spi
        freertos
        esp_timer
        heap
        log
)
```

---

## 8. 集成步骤

### Step 1: 在你的开发环境搭建 LCD 裸驱动

先不管项目流水线，单独测试：
- SPI 初始化 + 写命令/数据
- 填充颜色 → 看到纯色屏
- 画线、画矩形
- 显示 ASCII 字符

### Step 2: 创建 tft_display 组件

在 `Project/components/tft_display/` 下创建组件，在 `main/CMakeLists.txt` 的 `REQUIRES` 中添加 `tft_display`。

### Step 3: 定义全局显示数据结构

方法 A（推荐）：
- 在 `main.cpp` 中声明 `display_data_t g_display_data;`
- 在 `frame_handling_task` 的每个有效帧更新字段
- `tft_display` 组件用 `extern display_data_t g_display_data;` 读取

### Step 4: 实现 PERCLOS 历史曲线

- 维护一个浮点循环缓冲区（120 个点可显示约 12 分钟）
- 每收到新的 PERCLOS 值，添加到缓冲区并触发重绘

### Step 5: 状态面板显示

- 当前等级的文字+颜色指示
- 人脸状态图标
- 统计数字更新

### Step 6: 联合调试

将 ESP32-S3 接到实际摄像头信号源，观察 TFT 显示的 PERCLOS 曲线是否与串口日志一致。

---

## 9. 交付物清单

### 必须交付

- [ ] `components/tft_display/` 完整组件（带 CMakeLists.txt）
- [ ] TFT 屏幕点亮，显示测试图案
- [ ] PERCLOS 滚动曲线实时显示
- [ ] 疲劳等级颜色指示（绿/黄/红）
- [ ] 人脸检测状态指示
- [ ] SPI 接线图 + GPIO 分配说明

### 期望交付

- [ ] 驾驶统计面板（总帧数/闭眼帧数/闭眼率）
- [ ] 驾驶员身份显示
- [ ] 背光 PWM 调节

### 加分项

- [ ] LVGL 框架支持（按钮、菜单等）
- [ ] 报警弹窗（严重疲劳时闪烁）
- [ ] 多级菜单（设置界面）

---

## 10. 常见坑与注意事项

### 10.1 PSRAM 争用
- 眼部分类器的 Tensor Arena（820KB）和人脸识别的 Tensor Arena（3MB）都在 PSRAM
- 如果显示屏也用 PSRAM 做帧缓冲，注意分配合法性（`MALLOC_CAP_SPIRAM`）
- ESP32-S3 的 PSRAM SPI 总线与 LCD SPI 不冲突（LCD 用 SPI2，PSRAM 用 SPI1/专用 Octal）

### 10.2 USB DMA 冲突
- USB UVC 使用 DMA 访问内部 SRAM，不能访问 PSRAM
- **LCD SPI 使用 PSRAM 做帧缓冲是安全的**（LCD 不走 USB 总线）

### 10.3 任务优先级
- `usb_lib` 任务：优先级 15
- `uvc_frame` 任务：优先级 13（此处运行人脸检测+分类+PERCLOS）
- **建议 TFT 刷新任务优先级：`10~12`**，别抢 UVC 的 CPU 时间

### 10.4 避免阻塞
- TFT 刷新使用 DMA + 双缓冲，不要用阻塞式写屏
- SPI 传输可以用 `spi_device_transmit()`，配合 `spi_device_acquire_bus()` / `spi_device_release_bus()`

### 10.5 刷新频率
- PERCLOS 每 ~6 秒才更新一个有效值（30帧窗口 @ 5fps 处理速率）
- **不需要每帧刷屏**，每 1~2 秒刷新一次就够了
- 等级变化时才需要立即刷新

### 10.6 不要删除 build 目录
- 项目构建目录保留 cmake 缓存，删除后需要联网重新配置（国内网络不稳会卡 70~120 秒）
- 见 CLAUDE.md 详细说明

### 10.7 关于人脸识别
- 仅在首次检测到人脸时运行一次，耗时 ~12.6 秒
- 在其运行期间，PERCLOS 流水线暂停
- 显示屏在此期间可以显示"人脸识别中…"

---

## 11. 附录：数据流图

```
┌─────────────────────────────────────────────────────────────┐
│                    frame_handling_task (Core 1)              │
│                                                             │
│  每 10 帧解码 + 检测人脸                                       │
│        │                                                    │
│        ▼                                                    │
│  裁剪眼睛 ROI → 眼部分类                                      │
│        │                                                    │
│        ▼                                                    │
│  PERCLOS ← 是否闭眼?                                         │
│        │                                                    │
│        ▼                                                    │
│  更新 g_display_data 全局变量  ───── 轮询读取 ──────────────┐ │
│        │                                                    │ │
│        ▼                                                    │ │
│  level_change_cb → 语音播报                                   │ │
│                                                              │ │
└─────────────────────────────────────────────────────────────┘ │
                                                               │
┌─────────────────────────────────────────────────────────────┐ │
│                    tft_display_task (Core 0)                  │ │
│                                                             │ │
│  while(1) {                                                 │ │
│    vTaskDelay(1000ms)   ← 每 1 秒刷新一次                     │ │
│    read(g_display_data) ←──────────────────────────────────────┘
│    draw_perclos_curve()                                      │
│    draw_status_panel()                                       │
│    spi_transmit(...)      → TFT LCD                          │
│  }                                                           │
└─────────────────────────────────────────────────────────────┘
```

---

## 参考文件索引

| 文件 | 用途 |
|------|------|
| `main/main.cpp` | 主流水线，你需要插入数据读取点 |
| `main/perclos.hpp` | PERCLOS 计算器 API 文档 |
| `main/CMakeLists.txt` | 添加 `tft_display` 到 REQUIRES |
| `components/voice_prompter/` | 参考组件架构（CMakeLists 写法、C API 风格） |
| `components/eye_classifier/eye_classifier_infer.h` | 参考 C API 风格 |
| `sdkconfig.defaults` | ESP-IDF 配置（注意 SPIRAM 和 WDT 设置） |
| `partitions.csv` | 分区表（3.5MB app） |

---

> 如果有问题，随时沟通。PERCLOS 的 `g_display_data` 结构体定义我们可以一起讨论确定最终字段。
