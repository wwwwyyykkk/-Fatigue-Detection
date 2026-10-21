// ================================================================
//  驾盾 DriveSafe Guardian
//  ESP32-S3-DevKitC-1 + OV5640 + SSD1306 + MQ-3 + EG800K
//
//  Arduino IDE 配置：
//  - Board: ESP32S3 Dev Module
//  - PSRAM: OPI PSRAM  ← 必须开启
//  - Partition Scheme: Huge APP (3MB No OTA)
//  - CPU Frequency: 240MHz
//
//  依赖库（库管理器安装）：
//  - Adafruit SSD1306
//  - Adafruit GFX Library
//  - esp32-camera（乐鑫官方）
//
//  Edge Impulse 模型（手动导入 .zip）：
//  - face_recognition_inferencing  （人脸识别）
//  - fatigue_detection_inferencing （疲劳检测）
//  两个库不能同时 #include，采用串行加载策略，见下方说明
// ================================================================

// ─── 串行模型加载控制 ────────────────────────────────────────────
// 当前加载的模型：0=人脸识别，1=疲劳检测
// 通过编译宏控制，实际项目中用动态库加载替代（见 README）
// 这里先 include 人脸识别，疲劳检测在身份验证通过后通过
// esp_partition 动态加载（详见 load_fatigue_model()）
#define MODEL_FACE_RECOGNITION   // 注释此行换用疲劳模型

#ifdef MODEL_FACE_RECOGNITION
  #include <face_recognition_inferencing.h>
#else
  #include <fatigue_detection_inferencing.h>
#endif

#include "esp_camera.h"
#include "config.h"
#include "state_machine.h"
#include "oled_display.h"
#include "buzzer.h"
#include "mq3_alcohol.h"
#include "eg800k_gps.h"
#include "net_report.h"

// ─── 运行阶段枚举 ────────────────────────────────────────────────
typedef enum {
    PHASE_FACE_VERIFY = 0,   // 人脸识别阶段（使用人脸模型）
    PHASE_DRIVING     = 1    // 行驶监测阶段（使用疲劳模型）
} RunPhase;

static RunPhase runPhase = PHASE_FACE_VERIFY;

// ─── 摄像头帧缓冲 ────────────────────────────────────────────────
static uint8_t* imgBuf = NULL;    // 96x96 灰度图缓冲

// ─── 行程相关 ────────────────────────────────────────────────────
static bool     tripStarted   = false;
static uint32_t faceVerifyStart = 0;
static uint32_t fatigueEventStart = 0;  // FATIGUE 进入时间（用于计时持续时长）

// ================================================================
//  摄像头初始化
// ================================================================
static bool camera_init() {
    camera_config_t cfg;
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.pin_pwdn     = CAM_PWDN;
    cfg.pin_reset    = CAM_RESET;
    cfg.pin_xclk     = CAM_XCLK;
    cfg.pin_sccb_sda = CAM_SIOD;
    cfg.pin_sccb_scl = CAM_SIOC;
    cfg.pin_d7 = CAM_Y7; cfg.pin_d6 = CAM_Y6;
    cfg.pin_d5 = CAM_Y5; cfg.pin_d4 = CAM_Y4;
    cfg.pin_d3 = CAM_Y3; cfg.pin_d2 = CAM_Y2;
    cfg.pin_d1 = CAM_Y1; cfg.pin_d0 = CAM_Y0;
    cfg.pin_vsync    = CAM_VSYNC;
    cfg.pin_href     = CAM_HREF;
    cfg.pin_pclk     = CAM_PCLK;
    cfg.xclk_freq_hz = 20000000;

    // AI 推理需要 96x96 灰度图
    cfg.pixel_format = PIXFORMAT_GRAYSCALE;
    cfg.frame_size   = FRAMESIZE_96X96;
    cfg.jpeg_quality = 0;
    cfg.fb_count     = 1;
    cfg.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        Serial.printf("[CAM] 初始化失败: 0x%x\n", err);
        return false;
    }
    Serial.println("[CAM] 初始化成功");
    return true;
}

// ================================================================
//  捕获一帧并转为 EI 推理所需 signal_t
// ================================================================
static camera_fb_t* _fb = NULL;

static int get_signal_data(size_t offset, size_t length, float* out) {
    // 灰度图归一化到 [-1, 1]（Edge Impulse 标准）
    for (size_t i = 0; i < length; i++) {
        out[i] = ((float)_fb->buf[offset + i] / 127.5f) - 1.0f;
    }
    return 0;
}

static bool capture_frame() {
    if (_fb) { esp_camera_fb_return(_fb); _fb = NULL; }
    _fb = esp_camera_fb_get();
    return (_fb != NULL);
}

// ================================================================
//  人脸识别阶段推理
//  返回：true=验证通过，false=继续等待
// ================================================================
static bool run_face_inference() {
    if (!capture_frame()) {
        Serial.println("[FACE] 捕获帧失败");
        return false;
    }

    signal_t signal;
    signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
    signal.get_data     = &get_signal_data;

    ei_impulse_result_t result = {0};
    EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
    if (err != EI_IMPULSE_OK) {
        Serial.printf("[FACE] 推理错误: %d\n", err);
        return false;
    }

    // 找最高置信度的类别
    float maxConf  = 0;
    int   maxLabel = 0;
    for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        if (result.classification[i].value > maxConf) {
            maxConf  = result.classification[i].value;
            maxLabel = i;
        }
    }

    g.faceConfidence = maxConf;

    // 打印结果
    Serial.printf("[FACE] 最佳匹配: %s (%.2f)\n",
        result.classification[maxLabel].label, maxConf);

    // 阈值判断（label 0 通常是授权人脸，视训练时的类别顺序而定）
    // 注意：需根据 Edge Impulse 项目中的 label 名称调整
    bool isAuthorized = (strcmp(result.classification[maxLabel].label, "authorized") == 0)
                        && (maxConf >= FACE_CONF_THRESHOLD);

    oled_face_verify(maxConf, isAuthorized);
    return isAuthorized;
}

// ================================================================
//  疲劳检测阶段推理
//  输出 EAR / MAR，写入 g.earValue / g.marValue / g.faceDetected
// ================================================================
static void run_fatigue_inference() {
    if (!capture_frame()) {
        g.faceDetected = false;
        return;
    }

    signal_t signal;
    signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
    signal.get_data     = &get_signal_data;

    ei_impulse_result_t result = {0};
    EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
    if (err != EI_IMPULSE_OK) {
        g.faceDetected = false;
        return;
    }

    // ── 解析 5 点关键点模型输出 ──────────────────────────────────
    // Edge Impulse 关键点模型输出：[face_conf, left_eye_x, left_eye_y,
    //   right_eye_x, right_eye_y, mouth_left_x, mouth_left_y,
    //   mouth_right_x, mouth_right_y, nose_x, nose_y]
    // EAR / MAR 需要根据关键点坐标计算。
    // 简化方案：直接从 classification 标签读取预处理后的 EAR/MAR 值
    // （需在 Edge Impulse 项目中将 EAR/MAR 作为输出特征）
    
    float faceConf = 0, ear = 0.35f, mar = 0.0f;

    for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        const char* lbl = result.classification[i].label;
        float       val = result.classification[i].value;
        if (strcmp(lbl, "face_conf") == 0) faceConf = val;
        else if (strcmp(lbl, "ear")  == 0) ear      = val;
        else if (strcmp(lbl, "mar")  == 0) mar      = val;
    }

    // 人脸置信度过滤
    if (faceConf < 0.5f) {
        g.faceDetected = false;
        g.earValue     = 0.35f;  // 默认睁眼值
        g.marValue     = 0.0f;
        return;
    }

    g.faceDetected   = true;
    g.earValue       = ear;
    g.marValue       = mar;
    g.faceConfidence = faceConf;
}

// ================================================================
//  按键处理
// ================================================================
static bool _btnTripPrev = HIGH;
static bool _btnAckPrev  = HIGH;

static void handle_buttons() {
    // 行程按键（BTN_TRIP）
    bool tripNow = digitalRead(BTN_TRIP);
    if (_btnTripPrev == HIGH && tripNow == LOW) {
        if (!tripStarted) {
            // 开始行程
            tripStarted      = true;
            g.tripActive     = true;
            g.driveStartMs   = millis();
            g.totalDriveSec  = 0;
            g.eventCount     = 0;
            net_reset_trip();
            net_start_task();
            Serial.println("[BTN] 行程开始！");
            oled_show_state();
        } else {
            // 结束行程
            tripStarted  = false;
            g.tripActive = false;
            int score = calc_safety_score();
            net_send_trip_report(score);
            oled_show_score(score);
            buzzer_stop();
            Serial.printf("[BTN] 行程结束，安全评分: %d\n", score);
            delay(5000);  // 显示评分 5 秒
        }
    }
    _btnTripPrev = tripNow;

    // 疲劳确认按键（BTN_ACK）
    bool ackNow = digitalRead(BTN_ACK);
    if (_btnAckPrev == HIGH && ackNow == LOW) {
        if (g.state == STATE_FATIGUE) {
            g.driverAck = true;
            Serial.println("[BTN] 司机确认，等待 FATIGUE 解除...");
        }
    }
    _btnAckPrev = ackNow;
}

// ================================================================
//  4 小时强制报警处理（在主循环调用）
// ================================================================
static void handle_drive_time() {
    if (!g.tripActive) return;
    if (g.totalDriveSec >= DRIVE_TIME_ALARM_SEC) {
        // 持续鸣响，直到停车（停车由状态机切换到 RESTING 后 buzzer_stop）
        if (g.state == STATE_DRIVING || g.state == STATE_FATIGUE) {
            buzzer_alarm(FATIGUE_SEVERE);
        }
    }
}

// ================================================================
//  主检测 Task（优先级 5）
// ================================================================
static void main_detect_task(void* arg) {
    uint32_t frameCount = 0;

    while (true) {
        uint32_t frameStart = millis();

        // ── 解析 GPS ──────────────────────────────────────────────
        eg800k_parse_tick();

        // ── 按键处理 ──────────────────────────────────────────────
        handle_buttons();

        // ── 驾驶时长管理 ──────────────────────────────────────────
        handle_drive_time();

        // ── 蜂鸣器 tick ───────────────────────────────────────────
        buzzer_tick();

        // ── AI 推理 ───────────────────────────────────────────────
        if (runPhase == PHASE_FACE_VERIFY) {
            // 人脸识别阶段
            bool passed = run_face_inference();
            if (passed) {
                Serial.println("[MAIN] 身份验证通过！切换到疲劳检测模式");
                runPhase = PHASE_DRIVING;
                // 注意：实际使用时需在此处卸载人脸模型、加载疲劳模型
                // esp_camera_deinit() 无需重初始化，模型切换见 README
                oled_face_verify(g.faceConfidence, true);
                delay(2000);
            } else {
                // 超时检查
                if ((millis() - faceVerifyStart) >= FACE_VERIFY_TIMEOUT) {
                    buzzer_alarm(FATIGUE_MILD);
                    Serial.println("[MAIN] 人脸验证超时！");
                    faceVerifyStart = millis();  // 重置，继续尝试
                }
            }
        } else {
            // 疲劳检测阶段
            run_fatigue_inference();

            // 状态机更新
            state_update();

            // OLED 刷新（每 3 帧更新一次，约 600ms）
            if (frameCount % 3 == 0) {
                oled_show_state();
            }

            // FATIGUE 持续时间记录
            if (g.state == STATE_FATIGUE && fatigueEventStart == 0) {
                fatigueEventStart = millis();
            } else if (g.state != STATE_FATIGUE && fatigueEventStart != 0) {
                uint16_t dur = (millis() - fatigueEventStart) / 1000;
                log_event(g.fatigueLevel == FATIGUE_SEVERE ? 1 : 0, dur);
                fatigueEventStart = 0;
            }
        }

        frameCount++;

        // ── 控制帧率约 5fps（200ms/帧） ───────────────────────────
        uint32_t elapsed = millis() - frameStart;
        if (elapsed < 200) {
            vTaskDelay(pdMS_TO_TICKS(200 - elapsed));
        }
    }
}

// ================================================================
//  setup()
// ================================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n========================================");
    Serial.println("  驾盾 DriveSafe Guardian v1.0");
    Serial.println("========================================\n");

    // 1. 初始化全局状态
    state_init();

    // 2. 初始化 OLED
    if (!oled_init()) {
        Serial.println("OLED 初始化失败，继续运行...");
    }
    oled_splash();

    // 3. 初始化蜂鸣器
    buzzer_init();

    // 4. 初始化按键
    pinMode(BTN_TRIP, INPUT_PULLUP);
    pinMode(BTN_ACK,  INPUT_PULLUP);

    // 5. 初始化摄像头
    Serial.println("[MAIN] 初始化摄像头...");
    if (!camera_init()) {
        oled.clearDisplay();
        oled.println("CAM INIT FAIL");
        oled.display();
        while (1) delay(1000);
    }

    // 6. MQ-3 初始化（异步预热）
    mq3_init();

    // 7. 酒驾预检（阻塞，等待 MQ-3 预热 + 采样）
    Serial.println("[MAIN] 酒驾预检中...");
    bool alcoholOk = mq3_precheck(MQ3_WARMUP_MS + 5000);
    if (!alcoholOk) {
        Serial.println("[MAIN] 酒精超标！禁止启动");
        oled_alcohol_alarm(g.mq3Raw);
        buzzer_alarm(FATIGUE_SEVERE);
        while (1) delay(1000);  // 永久阻止，需要重启
    }
    Serial.println("[MAIN] 酒驾预检通过");

    // 8. 启动 MQ-3 后台采样任务
    mq3_start_task();

    // 9. 初始化 EG800K
    Serial.println("[MAIN] 初始化 EG800K...");
    eg800k_init();

    // 10. 记录人脸验证开始时间
    faceVerifyStart = millis();

    // 11. 启动主检测 Task
    Serial.println("[MAIN] 启动主检测任务...");
    xTaskCreate(main_detect_task, "MainDetect", STACK_MAIN, NULL, 5, NULL);

    Serial.println("[MAIN] 系统就绪，请面向摄像头进行身份验证\n");
}

// ================================================================
//  loop() — Arduino loop 留空，全部逻辑在 FreeRTOS Task 中
// ================================================================
void loop() {
    vTaskDelay(pdMS_TO_TICKS(10000));
}
