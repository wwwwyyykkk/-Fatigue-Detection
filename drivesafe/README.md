# 驾盾 DriveSafe Guardian — 部署说明

## 文件结构
```
drivesafe/
├── drivesafe.ino       # 主程序入口
├── config.h            # 全局引脚/参数配置
├── state_machine.h/.cpp # 五状态机 + 共享数据
├── oled_display.h      # SSD1306 显示驱动
├── buzzer.h            # 蜂鸣器 PWM 驱动
├── mq3_alcohol.h       # MQ-3 酒精传感器
├── eg800k_gps.h        # EG800K GPS/4G 驱动
└── net_report.h        # 网络上报任务
```

## Arduino IDE 配置
| 选项 | 值 |
|------|----|
| Board | ESP32S3 Dev Module |
| PSRAM | **OPI PSRAM** ← 必须开启 |
| Flash Size | 16MB |
| Partition Scheme | **Huge APP (3MB No OTA)** |
| CPU Frequency | 240MHz |
| Upload Speed | 921600 |

## 依赖库安装（库管理器）
- Adafruit SSD1306
- Adafruit GFX Library
- esp32-camera（乐鑫官方，通过 Board Manager 随 ESP32 包自动安装）

## Edge Impulse 模型说明

### 模型一：人脸识别
1. 在 Edge Impulse 创建图像分类项目
2. 类别设置：`authorized`（授权人脸）、`unknown`（陌生人）
3. 输入：96×96 灰度图
4. 导出 Arduino 库，命名为 `face_recognition_inferencing`
5. 在 Arduino IDE 中 Sketch → Include Library → Add .ZIP Library

### 模型二：疲劳检测
1. 在 Edge Impulse 创建回归/关键点项目
2. 输出标签：`face_conf`、`ear`（Eye Aspect Ratio）、`mar`（Mouth Aspect Ratio）
3. 训练数据：收集睁眼/闭眼/打哈欠面部图像，标注 EAR/MAR 值
4. 导出 Arduino 库，命名为 `fatigue_detection_inferencing`

### 串行加载策略
由于 ESP32-S3 内存限制，两个模型不能同时驻留：
1. 启动时编译并加载 `face_recognition_inferencing`
2. 身份验证通过后，修改 `drivesafe.ino` 顶部宏：
   - 注释 `#define MODEL_FACE_RECOGNITION`
   - 重新编译上传（实际产品中可用 OTA 实现动态切换）

## 配置修改
编辑 `config.h`：
```cpp
#define AMAP_KEY    "你的高德API Key"
#define SERVER_URL  "http://你的服务器IP/api/report"
#define MOCK_LAT    39.9087   // 室内演示用坐标（纬度）
#define MOCK_LNG    116.3975  // 室内演示用坐标（经度）
```

## 硬件接线速查
| 模块 | 引脚 |
|------|------|
| OV5640 SCL | GPIO4 |
| OV5640 SDA | GPIO2 |
| OV5640 PWDN | GPIO15 |
| OV5640 RST | GPIO5 |
| OV5640 VSYNC | GPIO6 |
| OV5640 HREF | GPIO7 |
| OV5640 PCLK | GPIO13 |
| OV5640 D0-D7 | GPIO11,9,8,10,12,18,17,16 |
| OLED SDA | GPIO2（与摄像头共用 I2C） |
| OLED SCL | GPIO4（与摄像头共用 I2C） |
| 蜂鸣器 | GPIO38 |
| MQ-3 AO | GPIO39 |
| EG800K TX | GPIO43 |
| EG800K RX | GPIO44 |
| 行程按键 | GPIO35 |
| 确认按键 | GPIO36 |

## 演示流程
1. 上电，等待 MQ-3 预热（30秒）+ 酒驾预检
2. 面向摄像头，等待人脸验证通过
3. 按 BTN_TRIP 开始行程
4. 持续闭眼 → 触发 MILD / SEVERE 疲劳报警
5. 睁眼 30 秒 + 按 BTN_ACK 解除 FATIGUE
6. 遮挡摄像头（车在动）→ 触发 UNKNOWN 离岗报警
7. 按 BTN_TRIP 结束行程，查看安全评分
