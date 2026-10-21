# SYN6288 无声音故障排查指南

## 问题现象
- 日志显示 `Frame sent: 54 bytes` 
- 但扬声器无任何声音输出
- UART TX 数据已发送

## 可能原因与排查步骤

### 1. 硬件连接问题（最常见）

#### 检查清单
```
[ ] SYN6288 VCC 接 5V（不是 3.3V！）
[ ] SYN6288 GND 接 ESP32 GND（必须共地）
[ ] SYN6288 RX  接 ESP32 GPIO6
[ ] SYN6288 BUSY 接 ESP32 GPIO7（可选，但建议接）
[ ] SYN6288 SPK+ / SPK- 接扬声器（8Ω 0.5W）
[ ] SYN6288 红色电源指示灯是否亮起
```

#### 常见错误
- ❌ VCC 接到 3.3V → 模块工作不稳定
- ❌ GND 未共地 → 电平不一致，无法通信
- ❌ GPIO6 接到 SYN6288 TX → 应该接 RX！
- ❌ 扬声器阻抗过高/过低 → 建议 8Ω

---

### 2. SYN6288 波特率配置错误

**出厂默认波特率可能不是 9600！**

#### 解决方法：重新配置波特率

**步骤 1：用 USB-TTL 模块直连 SYN6288**
```
USB-TTL TX  -> SYN6288 RX
USB-TTL RX  -> SYN6288 TX
USB-TTL 5V  -> SYN6288 VCC
USB-TTL GND -> SYN6288 GND
```

**步骤 2：打开串口助手，尝试以下波特率**
- 9600 bps
- 19200 bps
- 115200 bps

**步骤 3：发送测试指令（HEX 模式）**
```
FD 00 0E 01 00 5B 76 31 30 5D 6E 69 68 61 6F
```
解释：
- `FD` - 帧头
- `00 0E` - 数据长度 14 字节
- `01` - 语音合成命令
- `00` - 保留字节
- `5B 76 31 30 5D` - 控制标签 `[v10]` 音量
- `6E 69 68 61 6F` - 文本 "nihao" (拼音)

**如果某个波特率有声音，说明需要修改代码中的波特率配置。**

---

### 3. 软件 UART 时序问题

当前代码用 `esp_rom_delay_us(104)` 实现 9600 bps 软件 UART。

#### 可能的问题
- 中断干扰导致位时序不准
- CPU 频率配置错误

#### 解决方法：用逻辑分析仪验证

测量 GPIO6 的波形：
1. 空闲时应为高电平
2. 起始位为低电平，持续 104us
3. 8 个数据位，每位 104us
4. 停止位为高电平，持续 104us

如果时序偏差 >10%，需要调整 `BIT_US` 宏定义。

---

### 4. BUSY 引脚悬空

如果 BUSY 引脚没接，`syn6288_is_busy()` 返回值不可靠。

#### 临时解决方法：禁用 BUSY 检测

修改 `syn6288.c` 中的 `syn6288_wait_done()`：
```c
void syn6288_wait_done(void)
{
    // 固定等待 3 秒（绕过 BUSY 检测）
    vTaskDelay(pdMS_TO_TICKS(3000));
}
```

---

### 5. SYN6288 模块损坏

#### 验证方法
用 USB-TTL 直连 SYN6288 发送上述测试帧，如果仍无声音，说明模块本身有问题。

---

## 使用诊断工具

### 编译并烧录
```powershell
cd D:\Fatigue-detection\VirtualGPS
.\activate.ps1
idf.py build flash monitor
```

### 运行诊断
启动后在串口输入：
```
test_syn
```

### 查看输出
```
=== GPIO Config Check ===
GPIO6 (TX) mode: 1 (should be OUTPUT=1)
GPIO6 (TX) level: 1 (should be HIGH=1 when idle)
GPIO7 (BUSY) mode: 0 (should be INPUT=0)
GPIO7 (BUSY) level: 0 (LOW=0 means not playing)

=== Sending Test Frame ===
...
BUSY before send: 0
BUSY after 50ms: 1 (should be HIGH=1 if playing)
```

---

## 预期结果

### 正常情况
1. GPIO6 配置为输出，空闲时为高电平
2. 发送帧后 50-100ms 内 BUSY 变高
3. 播放完成后 BUSY 变低
4. **扬声器发出声音**

### 异常情况
| 现象 | 可能原因 |
|------|---------|
| BUSY 始终为 0 | BUSY 未接线，或 SYN6288 未收到有效数据 |
| BUSY 变高但无声音 | 扬声器未接线，或 SYN6288 损坏 |
| GPIO6 电平不变 | 软件 UART 初始化失败 |

---

## 快速验证：手动发送测试帧

在 `app_main()` 最后添加：
```c
// 每 5 秒播放一次测试音
while (1) {
    ESP_LOGI(TAG, ">>> Manual test: speaking...");
    syn6288_speak(SYN_VOICE_SYS_BOOT);
    vTaskDelay(pdMS_TO_TICKS(5000));
}
```

如果循环播放仍无声音，**100% 是硬件问题**。

---

## 最终建议

1. **先用 USB-TTL 直连测试 SYN6288**，确认模块本身工作正常
2. **用万用表测量 VCC 是否为 5V**
3. **用逻辑分析仪抓取 GPIO6 波形**，验证时序
4. **如果以上都正常，考虑更换 SYN6288 模块**
