# 虚拟GPS模拟板 — 驾盾疲劳驾驶检测系统

ESP32-S3 虚拟GPS/驾驶场景模拟器，用于答辩演示。

---

## 功能

| 功能 | 硬件 | 说明 |
|------|------|------|
| 按键模拟 | 6 个 GPIO 按键 | 速度/地点/司机/驾驶时长 |
| OLED 显示 | SSD1306 I2C 128×64 | 实时状态 |
| TTS 语音 | **SYN6288** 软件UART | 超速/疲劳/服务区等语音告警 |
| I2S 语音 | MAX98357 | boot.wav 开机提示音 |
| UART 主控通信 | UART2 (GPIO17 TX) | 每秒发送状态到主控板 |
| GPS 预留 | UART1 (GPIO15/16) | 空闲可用，原生 IO_MUX |

## 硬件接线

### GPIO 分配总表

```
按键1-6:       GPIO8 / 9 / 11 / 12 / 2 / 16
OLED I2C:      GPIO4 (SDA) / GPIO5 (SCL)
SYN6288 TTS:   GPIO13 (软件UART TX) / GPIO7 (BUSY)
MAX98357 I2S:  GPIO18 (BCK) / GPIO19 (LRC) / GPIO20 (DIN)
UART2 → 主控:   GPIO17 (TX)
UART1 → GPS:   GPIO15 (TX) / GPIO16 (RX) — 空闲可选
```

### 接线检查清单
- [ ] SYN6288 VCC 必须接 **5V**（不能 3.3V）
- [ ] 所有模块 GND 共地
- [ ] MAX98357 VIN 接 **5V**
- [ ] 按键一端接 GPIO，另一端接 GND（内部上拉）

## 编译 & 烧录

### 环境要求
- ESP-IDF **v5.5.x**（v5.5.4 测试通过）
- Windows PowerShell（Win10/11 自带）
- 芯片: **ESP32-S3**

### 队友拿到压缩包后，完整操作步骤

#### 步骤 1：解压
```
右键 → 解压到 C:\VirtualGPS\（不要解压到中文路径）
```

#### 步骤 2：修改激活脚本
用记事本打开 `C:\VirtualGPS\activate.ps1`，把第一行路径改成你电脑上 ESP-IDF 的实际安装位置：
```powershell
# 改这一行！
. D:\ESP\.espressif\v5.5.4\esp-idf\export.ps1
         ↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑
# 改成你的 ESP-IDF 安装路径，例如:
# . C:\Espressif\frameworks\esp-idf-v5.5.4\export.ps1
```

#### 步骤 3：打开 PowerShell
```
Win+R → 输入 powershell → 回车
```

#### 步骤 4：激活环境
```powershell
cd C:\VirtualGPS
.\activate.ps1
```
看到绿色 `Done! You can now compile ESP-IDF projects.` 即成功。

#### 步骤 5：设目标芯片（仅首次，之后跳过）
```powershell
idf.py set-target esp32s3
```

#### 步骤 6：编译
```powershell
idf.py build
```
预计 1~2 分钟，最后一行显示 `Project build complete.` 即编译成功。

#### 步骤 7：烧录 + 监视
```powershell
idf.py -p COM3 flash monitor
#              ↑↑↑↑ 改成你的实际 COM 口
```
如何查 COM 口：设备管理器 → 端口(COM和LPT) → 找 `USB-SERIAL CH340` 或类似名称。

按 **Ctrl + ]** 退出监视器。

#### 步骤 8（可选）：只监视不烧录
```powershell
idf.py -p COM3 monitor
```

---

### 故障排查

| 现象 | 解决 |
|------|------|
| `'idf.py' 不是内部命令` | 没运行 `.\activate.ps1`，重做步骤 4 |
| `idf.py` 卡 `Running cmake` 不动 | 先执行 `$env:IDF_COMPONENT_MANAGER_ENABLED = "0"` 再 `idf.py build` |
| `UnicodeEncodeError: 'gbk' ...` | 先执行 `$env:PYTHONIOENCODING = "utf-8"` 再编译 |
| 找不到 COM 口 | 检查 USB 线是否插好，设备管理器刷新 |
| 烧录失败 `A fatal error occurred` | 按住板子 BOOT 键不放 → 按一下 RST → 松开 BOOT → 重试烧录 |
| 烧录失败 `wrong chip` | 确认是 ESP32-S3，不是 ESP32/S2/C3 |

## 按键功能 & 语音播报

| 按键 | 功能 | 播报语音 |
|------|------|---------|
| SW1 (GPIO8) | 速度 = 0 km/h | — |
| SW2 (GPIO9) | 速度 = 80 km/h | — |
| SW3 (GPIO11) | 速度 = 150 km/h（超速） | "检测到超速，请减速行驶" |
| SW4 (GPIO12) | 地点循环：普通→服务区→应急车道 | "当前位置为服务区" / "应急车道禁止停车" |
| SW5 (GPIO2) | 司机 1↔2 切换 | "司机身份已切换" |
| SW6 (GPIO16) | 驾驶时长 +60min | 3h/3.5h/4h 疲劳告警 |

### 开机
- MAX98357 播放 `boot.wav` 提示音
- SYN6288 播报 "系统初始化完成"

## 串口命令

烧录后在 monitor 输入:

```
test_syn   → 运行 SYN6288 诊断
scan_baud  → SYN6288 波特率扫描
say_hi     → 发送测试语音
```

## UART 协议

### VirtualGPS → 主控板 (每秒心跳)
```
速度,地点类型,司机ID,驾驶时长(秒)\n

示例: 80,1,1,200\n
```
- 地点: 0=普通路段, 1=服务区, 2=应急车道

## 代码结构

```
VirtualGPS/
├── main/
│   ├── main.c              ← 入口，任务创建，串口命令
│   ├── config.h            ← 全局配置（引脚/参数集中管理）
│   ├── gps_state.h/.c      ← 状态机（线程安全，信号量保护）
│   ├── buttons_gpio.h/.c   ← GPIO 按键驱动
│   ├── uart_protocol.h/.c  ← UART2 通信协议
│   ├── oled_display.h/.c   ← SSD1306 OLED 驱动+显示
│   ├── syn6288.h/.c        ← SYN6288 TTS 驱动（软件UART, 官方协议）
│   ├── syn6288_voices.h    ← 9 条预设语音（GB2312）
│   ├── syn6288_test.h/.c   ← 诊断兼容层
│   ├── chinese_font.h      ← OLED 中文字体
│   └── CMakeLists.txt
├── components/
│   └── voice_prompter/     ← MAX98357 I2S 语音播报
├── tools/
│   ├── gen_chinese_font.py ← 中文字体生成工具
│   └── chinese_font.h     ← 字体源文件
├── activate.ps1            ← 一键激活 ESP-IDF 环境
├── CMakeLists.txt          ← 顶层 CMake
├── sdkconfig               ← ESP-IDF 配置
├── sdkconfig.defaults      ← 默认配置覆盖
└── README.md
```

## SYN6288 注意事项

- 出厂默认 **9600 bps** 8N1
- 帧格式: `FD + 长度(2) + 0x01 + 0x01 + 文本 + XOR校验`
- 编码: GBK（`0x01`），非 GB2312
- **必须带 XOR 校验和**，否则模块不响应
- 官方参考: `E:\Fatigue dataset\1778751105332853\`
- 当前配置: `[m4]` 背景音中 / `[v2]` 音量 / `[t5]` 语速

## 许可
答辩演示项目，代码开源供学习参考。
