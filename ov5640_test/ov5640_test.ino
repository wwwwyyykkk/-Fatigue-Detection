/*
 * ESP32-S3 + 正点原子 OV5640 摄像头诊断测试程序
 * 
 * 接线（请严格核对）:
 * OV5640模组引脚  ->  ESP32-S3 GPIO
 * -----------------------------------------
 * 3.3V            ->  3.3V
 * GND             ->  GND
 * SCL             ->  GPIO4   (I2C时钟，需4.7K上拉到3.3V)
 * SDA             ->  GPIO2   (I2C数据，需4.7K上拉到3.3V)
 * PWDN            ->  GPIO15  (低电平工作，高电平掉电)
 * RST             ->  GPIO5   (低电平复位)
 * VSYNC           ->  GPIO6
 * HREF            ->  GPIO7
 * PCLK            ->  GPIO13
 * D0              ->  GPIO11
 * D1              ->  GPIO9
 * D2              ->  GPIO8
 * D3              ->  GPIO10
 * D4              ->  GPIO12
 * D5              ->  GPIO18
 * D6              ->  GPIO17
 * D7              ->  GPIO16
 * XCLK            ->  GPIO14  (由ESP32-S3内部生成，不需外部时钟源)
 * FLASH           ->  GPIO3   (可不接)
 * 
 * Arduino IDE 配置:
 * - 开发板: "ESP32S3 Dev Module"
 * - PSRAM: "OPI PSRAM"  ← 非常重要！
 * - Flash Size: "16MB" 或按实际
 * - Partition Scheme: "Huge APP (3MB No OTA)"
 * - CPU Frequency: "240MHz"
 * - Upload Speed: "921600"
 */

#include "esp_camera.h"

// ─── 引脚定义 ─────────────────────────────────
#define PWDN_GPIO_NUM     15
#define RESET_GPIO_NUM     5
#define XCLK_GPIO_NUM     14
#define SIOD_GPIO_NUM      2
#define SIOC_GPIO_NUM      4
#define Y7_GPIO_NUM       16
#define Y6_GPIO_NUM       17
#define Y5_GPIO_NUM       18
#define Y4_GPIO_NUM       12
#define Y3_GPIO_NUM       10
#define Y2_GPIO_NUM        8
#define Y1_GPIO_NUM        9
#define Y0_GPIO_NUM       11
#define VSYNC_GPIO_NUM     6
#define HREF_GPIO_NUM      7
#define PCLK_GPIO_NUM     13

// ─── 辅助：打印内存状态 ────────────────────────
void printMemory(const char* label) {
  Serial.printf("[内存] %s\n", label);
  Serial.printf("  内部RAM可用: %6d KB\n", ESP.getFreeHeap() / 1024);
  Serial.printf("  内部RAM最大块: %4d KB\n", ESP.getMaxAllocHeap() / 1024);
  if (ESP.getPsramSize() > 0) {
    Serial.printf("  PSRAM总量:   %6d KB\n", ESP.getPsramSize() / 1024);
    Serial.printf("  PSRAM可用:   %6d KB\n", ESP.getFreePsram() / 1024);
  } else {
    Serial.println("  PSRAM: 未检测到！");
  }
  Serial.println();
}

// ─── PWDN / RST 上电时序 ─────────────────────
void powerOnSequence() {
  Serial.println("[上电] 执行OV5640上电时序...");

  pinMode(PWDN_GPIO_NUM, OUTPUT);
  pinMode(RESET_GPIO_NUM, OUTPUT);

  // 1. 先掉电
  digitalWrite(PWDN_GPIO_NUM, HIGH);
  delay(100);

  // 2. 释放复位（保持低）
  digitalWrite(RESET_GPIO_NUM, LOW);
  delay(100);

  // 3. 上电（PWDN拉低 = 工作状态）
  digitalWrite(PWDN_GPIO_NUM, LOW);
  delay(100);

  // 4. 释放复位
  digitalWrite(RESET_GPIO_NUM, HIGH);
  delay(200);  // 等待sensor稳定

  Serial.println("[上电] 完成，等待200ms稳定");
}

// ─── 摄像头初始化（自动选择PSRAM/DRAM）──────────
bool initCamera() {
  bool hasPsram = (ESP.getPsramSize() > 0);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_d7       = Y7_GPIO_NUM;
  config.pin_d6       = Y6_GPIO_NUM;
  config.pin_d5       = Y5_GPIO_NUM;
  config.pin_d4       = Y4_GPIO_NUM;
  config.pin_d3       = Y3_GPIO_NUM;
  config.pin_d2       = Y2_GPIO_NUM;
  config.pin_d1       = Y1_GPIO_NUM;
  config.pin_d0       = Y0_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;

  config.xclk_freq_hz = 20000000;   // 20MHz，比16MHz更稳定
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_QVGA;  // 320x240，最保守
  
  //config.pixel_format = PIXFORMAT_GRAYSCALE;
  //config.frame_size   = FRAMESIZE_96X96;//96x96灰度图

  config.jpeg_quality = 10;              // 0-63，数字越小质量越高

  if (hasPsram) {
    Serial.println("[摄像头] 检测到PSRAM，使用PSRAM模式（推荐）");
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.fb_count    = 2;
    config.grab_mode   = CAMERA_GRAB_LATEST;
  } else {
    Serial.println("[摄像头] 未检测到PSRAM，使用DRAM模式（内存紧张）");
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count    = 1;
    config.grab_mode   = CAMERA_GRAB_WHEN_EMPTY;
  }

  Serial.println("[摄像头] 正在初始化...");
  esp_err_t err = esp_camera_init(&config);

  if (err != ESP_OK) {
    Serial.printf("[摄像头] 初始化失败！错误码: 0x%x\n\n", err);
    // 常见错误码说明
    if (err == 0x20005)      Serial.println("  → 0x20005: I2C通信失败，检查SCL/SDA接线和上拉电阻");
    else if (err == 0x101)   Serial.println("  → 0x101: 参数错误，检查引脚定义");
    else if (err == 0x103)   Serial.println("  → 0x103: 内存分配失败，启用PSRAM");
    else                     Serial.println("  → 未知错误，检查供电和接线");
    return false;
  }

  // 读取sensor信息
  sensor_t* s = esp_camera_sensor_get();
  if (!s) {
    Serial.println("[摄像头] 获取sensor句柄失败！");
    return false;
  }

  Serial.printf("[摄像头] 初始化成功！Sensor PID: 0x%04X", s->id.PID);
  if (s->id.PID == 0x5640) Serial.println(" → OV5640 ✓");
  else                     Serial.printf(" → 警告：期望0x5640，实际0x%04X\n", s->id.PID);

  // 基础画质设置
  s->set_brightness(s, 0);
  s->set_contrast(s, 0);
  s->set_saturation(s, 0);
  s->set_sharpness(s, 0);
  s->set_whitebal(s, 1);     // 自动白平衡
  s->set_exposure_ctrl(s, 1); // 自动曝光

  return true;
}

// ─── 捕获并验证一帧 ──────────────────────────
void captureAndCheck() {
  Serial.println("\n[捕获] 开始...");
  printMemory("捕获前");

  camera_fb_t* fb = esp_camera_fb_get();

  if (!fb) {
    Serial.println("[捕获] 失败！esp_camera_fb_get() 返回NULL");
    Serial.println("  可能原因:");
    Serial.println("  1. 内存不足（PSRAM未启用）");
    Serial.println("  2. 摄像头无输出信号（VSYNC/PCLK/HREF接线问题）");
    Serial.println("  3. XCLK频率不匹配");
    return;
  }

  Serial.println("[捕获] 成功！");
  Serial.printf("  分辨率: %d x %d\n", fb->width, fb->height);
  Serial.printf("  大小:   %d bytes (%.1f KB)\n", fb->len, fb->len / 1024.0f);
  Serial.printf("  格式:   %s\n", fb->format == PIXFORMAT_JPEG ? "JPEG" : "RAW");

  // 验证JPEG文件头
  if (fb->len > 4) {
    bool validJpeg = (fb->buf[0] == 0xFF && fb->buf[1] == 0xD8 &&
                      fb->buf[fb->len - 2] == 0xFF && fb->buf[fb->len - 1] == 0xD9);
    Serial.printf("  JPEG完整性: %s\n", validJpeg ? "✓ 完整" : "✗ 不完整（数据异常）");

    // 打印前16字节用于诊断
    Serial.print("  数据头(hex): ");
    for (int i = 0; i < 16 && i < (int)fb->len; i++) {
      Serial.printf("%02X ", fb->buf[i]);
    }
    Serial.println();
  }
  


  // 将 JPEG 数据以 Base64 编码通过串口发出
Serial.println("==IMG_START==");
// Base64 编码表
const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
uint8_t* data = fb->buf;
size_t   len  = fb->len;
for (size_t i = 0; i < len; i += 3) {
    uint8_t b0 = data[i];
    uint8_t b1 = (i+1 < len) ? data[i+1] : 0;
    uint8_t b2 = (i+2 < len) ? data[i+2] : 0;
    Serial.write(b64[b0 >> 2]);
    Serial.write(b64[((b0 & 3) << 4) | (b1 >> 4)]);
    Serial.write((i+1 < len) ? b64[((b1 & 0xF) << 2) | (b2 >> 6)] : '=');
    Serial.write((i+2 < len) ? b64[b2 & 0x3F] : '=');
    if ((i / 3 % 19) == 18) Serial.println(); // 每76字符换行
}
Serial.println();
Serial.println("==IMG_END==");


  esp_camera_fb_return(fb);
  printMemory("捕获后");
}

// ─── 切换分辨率（修复版）────────────────────────
void switchResolution() {
  static int res = 0;
  sensor_t* s = esp_camera_sensor_get();
  if (!s) { Serial.println("[分辨率] 获取sensor失败"); return; }

  const framesize_t sizes[] = {
    FRAMESIZE_QVGA,    // 320x240
    FRAMESIZE_VGA,     // 640x480
    FRAMESIZE_SVGA,    // 800x600
    FRAMESIZE_HD       // 1280x720
  };
  const char* names[] = { "QVGA(320x240)", "VGA(640x480)", "SVGA(800x600)", "HD(1280x720)" };

  res = (res + 1) % 4;
  Serial.printf("\n[分辨率] 切换到: %s\n", names[res]);

  // 注意：VGA及以上需要PSRAM，否则会失败
  if (res > 0 && ESP.getPsramSize() == 0) {
    Serial.println("  警告：未检测到PSRAM，高分辨率可能失败！");
  }

  esp_err_t err = s->set_framesize(s, sizes[res]);
  Serial.printf("  结果: %s\n", err == ESP_OK ? "成功" : "失败");
}

// ─── Setup ───────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(2000);  // 等待串口稳定

  Serial.println("\n========================================");
  Serial.println("  ESP32-S3 + OV5640 摄像头诊断程序");
  Serial.println("========================================\n");

  // Step 1: 内存检查
  printMemory("启动时");

  // PSRAM检查提示
  if (ESP.getPsramSize() == 0) {
    Serial.println("⚠ 警告：未检测到PSRAM！");
    Serial.println("  请在Arduino IDE中设置: Tools → PSRAM → OPI PSRAM");
    Serial.println("  如果硬件确实没有PSRAM，QVGA捕获可能失败\n");
  }

  // Step 2: 上电时序
  powerOnSequence();

  // Step 3: 初始化摄像头
  if (!initCamera()) {
    Serial.println("\n========================================");
    Serial.println("  初始化失败，请检查接线后重启");
    Serial.println("========================================");
    while (1) delay(1000);
  }

  // Step 4: 尝试第一次捕获
  delay(500);  // 等待sensor稳定
  captureAndCheck();

  // Step 5: 打印命令
  Serial.println("\n========================================");
  Serial.println("命令: c=捕获  s=切换分辨率  m=内存  h=帮助");
  Serial.println("========================================\n");
}

// ─── Loop ────────────────────────────────────
void loop() {
  if (Serial.available()) {
    char cmd = Serial.read();
    switch (cmd) {
      case 'c': captureAndCheck();    break;
      case 's': switchResolution();   break;
      case 'm': printMemory("当前");  break;
      case 'h':
        Serial.println("c=捕获  s=切换分辨率  m=内存  h=帮助");
        break;
    }
  }
}
