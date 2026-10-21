/*
 * 驾盾·长途卫士 — 标定+疲劳检测合并版
 *
 * 串口命令：
 *   mode:calib   切换到标定模式（实时传图+显示裁剪框）
 *   mode:detect  切换到疲劳检测模式
 *   e:x,y,w,h    调整眼部裁剪框（像素值）
 *   m:x,y,w,h    调整嘴部裁剪框（像素值）
 *   p            暂停/继续
 *   r            重置统计
 *   s            查看内存
 *   h            帮助
 */

#include <Fatigue-Detection_inferencing.h>
#include "esp_camera.h"

// ─── 摄像头引脚 ───────────────────────────────────────────────────
#define CAM_PWDN   15
#define CAM_RESET   5
#define CAM_XCLK   14
#define CAM_SIOD    2
#define CAM_SIOC    4
#define CAM_Y7     16
#define CAM_Y6     17
#define CAM_Y5     18
#define CAM_Y4     12
#define CAM_Y3     10
#define CAM_Y2      8
#define CAM_Y1      9
#define CAM_Y0     11
#define CAM_VSYNC   6
#define CAM_HREF    7
#define CAM_PCLK   13

// ─── 裁剪框（像素值，标定后直接改这里） ─────────────────────────
int EYE_X   = 124;
int EYE_Y   =  52;
int EYE_W   =  73;
int EYE_H   =  30;

int MOUTH_X = 131;
int MOUTH_Y =  83;
int MOUTH_W =  58;
int MOUTH_H =  35;

// ─── 检测阈值 ─────────────────────────────────────────────────────
#define EYE_CLOSED_THRESHOLD  0.65f
#define YAWN_THRESHOLD        0.75f
#define YAWN_DURATION_MS      2500
#define PERCLOS_MILD          15.0f
#define PERCLOS_SEVERE        30.0f

// ─── 运行模式 ─────────────────────────────────────────────────────
typedef enum { MODE_CALIB, MODE_DETECT } RunMode;
static RunMode mode    = MODE_CALIB;  // 默认先标定
static bool    running = true;
static String pending_save = "";

// ─── 统计变量 ─────────────────────────────────────────────────────
static int      frameCount  = 0;
static int      closedCount = 0;
static int      yawnTotal   = 0;
static bool     inYawn      = false;
static uint32_t yawnStartMs = 0;

// ─── 图像缓冲 ─────────────────────────────────────────────────────
static camera_fb_t* fb        = NULL;
static uint8_t      crop96[96*96];
static uint8_t*     g_infer   = NULL;

// ================================================================
//  摄像头初始化（format 可切换 JPEG / RGB565）
// ================================================================
static bool camera_init(pixformat_t format) {
    if (fb) { esp_camera_fb_return(fb); fb = NULL; }
    esp_camera_deinit();
    delay(100);

    camera_config_t cfg;
    cfg.ledc_channel=LEDC_CHANNEL_0; cfg.ledc_timer=LEDC_TIMER_0;
    cfg.pin_pwdn=CAM_PWDN;   cfg.pin_reset=CAM_RESET;
    cfg.pin_xclk=CAM_XCLK;
    cfg.pin_sccb_sda=CAM_SIOD; cfg.pin_sccb_scl=CAM_SIOC;
    cfg.pin_d7=CAM_Y7; cfg.pin_d6=CAM_Y6; cfg.pin_d5=CAM_Y5;
    cfg.pin_d4=CAM_Y4; cfg.pin_d3=CAM_Y3; cfg.pin_d2=CAM_Y2;
    cfg.pin_d1=CAM_Y1; cfg.pin_d0=CAM_Y0;
    cfg.pin_vsync=CAM_VSYNC; cfg.pin_href=CAM_HREF; cfg.pin_pclk=CAM_PCLK;
    cfg.xclk_freq_hz = 20000000;
    cfg.pixel_format = format;
    cfg.frame_size   = FRAMESIZE_QVGA;
    cfg.jpeg_quality = 12;
    cfg.fb_count     = 1;
    cfg.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
    cfg.fb_location  = CAMERA_FB_IN_DRAM;

    pinMode(CAM_PWDN,OUTPUT); pinMode(CAM_RESET,OUTPUT);
    digitalWrite(CAM_PWDN,HIGH); delay(100);
    digitalWrite(CAM_RESET,LOW);  delay(100);
    digitalWrite(CAM_PWDN,LOW);   delay(100);
    digitalWrite(CAM_RESET,HIGH); delay(200);

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        Serial.printf("[CAM] 失败: 0x%x\n", err);
        return false;
    }
    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        s->set_whitebal(s, 1);
        s->set_exposure_ctrl(s, 1);
        s->set_gain_ctrl(s, 1);
    }
    Serial.printf("[CAM] 成功 (%s)\n",
        format==PIXFORMAT_JPEG ? "JPEG" : "RGB565");
    return true;
}

// ================================================================
//  标定模式：发送 JPEG + 裁剪框参数
// ================================================================
static void calib_send_frame() {
    if (fb) { esp_camera_fb_return(fb); fb = NULL; }
    fb = esp_camera_fb_get();
    if (!fb) return;

    // 发送裁剪框参数（网页解析用）
    Serial.printf("##BOX:%d,%d,%d,%d,%d,%d,%d,%d##\n",
        EYE_X, EYE_Y, EYE_W, EYE_H,
        MOUTH_X, MOUTH_Y, MOUTH_W, MOUTH_H);

    // Base64 发送 JPEG
    const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    Serial.println("==IMG_START==");
    uint8_t* d = fb->buf;
    size_t   len = fb->len;
    for (size_t i = 0; i < len; i += 3) {
        uint8_t b0=d[i], b1=(i+1<len)?d[i+1]:0, b2=(i+2<len)?d[i+2]:0;
        Serial.write(b64[b0>>2]);
        Serial.write(b64[((b0&3)<<4)|(b1>>4)]);
        Serial.write((i+1<len) ? b64[((b1&0xF)<<2)|(b2>>6)] : '=');
        Serial.write((i+2<len) ? b64[b2&0x3F] : '=');
        if ((i/3 % 19) == 18) Serial.println();
    }
    Serial.println("\n==IMG_END==");
}

// ================================================================
//  检测模式：RGB565 裁剪 + 缩放到 96×96 灰度
// ================================================================
static bool crop_to_gray96(int x, int y, int w, int h) {
    if (!fb) return false;
    int sw=fb->width, sh=fb->height;
    x=max(0,min(x,sw-1)); y=max(0,min(y,sh-1));
    w=max(1,min(w,sw-x)); h=max(1,min(h,sh-y));

    for (int oy=0; oy<96; oy++) {
        for (int ox=0; ox<96; ox++) {
            int sx = x+(int)((float)ox/96.0f*w);
            int sy = y+(int)((float)oy/96.0f*h);
            sx=min(sx,sw-1); sy=min(sy,sh-1);
            int      idx = (sy*sw+sx)*2;
            uint16_t p   = ((uint16_t)fb->buf[idx]<<8)|fb->buf[idx+1];
            uint8_t  r   = ((p>>11)&0x1F)<<3;
            uint8_t  g   = ((p>> 5)&0x3F)<<2;
            uint8_t  b   = ( p     &0x1F)<<3;
            crop96[oy*96+ox] = (uint8_t)(0.299f*r+0.587f*g+0.114f*b);
        }
    }
    return true;
}

// ─── EI 推理回调 ──────────────────────────────────────────────────
static int ei_get_data(size_t offset, size_t length, float* out) {
    for (size_t i=0; i<length; i++)
        out[i] = ((float)g_infer[offset+i]/127.5f) - 1.0f;
    return 0;
}

static bool run_inference(float& conf_closed,
                           float& conf_open,
                           float& conf_yawn) {
    g_infer = crop96;
    signal_t sig;
    sig.total_length = 96*96;
    sig.get_data     = &ei_get_data;

    ei_impulse_result_t result = {0};
    if (run_classifier(&sig, &result, false) != EI_IMPULSE_OK)
        return false;

    conf_closed = result.classification[0].value;  // eye_closed
    conf_open   = result.classification[1].value;  // eye_open
    conf_yawn   = result.classification[2].value;  // yawning
    return true;
}

// ─── 打印进度条 ───────────────────────────────────────────────────
static void print_bar(const char* label, float val, float thr) {
    int  n = (int)(val*20);
    char bar[23]; bar[0]='[';
    for (int i=0;i<20;i++) bar[i+1]=(i<n)?'#':'-';
    bar[21]=']'; bar[22]='\0';
    Serial.printf("  %-12s%s %.2f%s\n",
        label, bar, val, (val>=thr)?" <<<":"");
}

// ================================================================
//  疲劳检测主循环
// ================================================================
static void detect_loop() {
    if (fb) { esp_camera_fb_return(fb); fb=NULL; }
    fb = esp_camera_fb_get();
    if (!fb) { Serial.println("[CAM] 捕获失败"); delay(500); return; }

    frameCount++;
    Serial.printf("\n─── Frame #%d ───────────────────\n", frameCount);
    Serial.printf("眼部框:(%d,%d) %dx%d  嘴部框:(%d,%d) %dx%d\n",
        EYE_X,EYE_Y,EYE_W,EYE_H, MOUTH_X,MOUTH_Y,MOUTH_W,MOUTH_H);

    // 眼部推理
    float conf_closed=0, conf_open=0, conf_yawn_e=0;
    Serial.println("[眼部]");
    if (crop_to_gray96(EYE_X, EYE_Y, EYE_W, EYE_H))
        run_inference(conf_closed, conf_open, conf_yawn_e);
    const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    // 采集保存逻辑
    if (pending_save != "") {
    String label = pending_save;
    pending_save = "";

    if (label == "yawn") {
        // 哈欠用嘴部图，重新裁剪
        crop_to_gray96(MOUTH_X, MOUTH_Y, MOUTH_W, MOUTH_H);
    }
    // 眼部图直接用当前 crop96

    
    Serial.printf("==SAVE_START:%s==\n", label.c_str());
    for (int i = 0; i < 96*96; i += 3) {
        uint8_t b0=crop96[i];
        uint8_t b1=(i+1<96*96)?crop96[i+1]:0;
        uint8_t b2=(i+2<96*96)?crop96[i+2]:0;
        Serial.write(b64[b0>>2]);
        Serial.write(b64[((b0&3)<<4)|(b1>>4)]);
        Serial.write((i+1<96*96)?b64[((b1&0xF)<<2)|(b2>>6)]:'=');
        Serial.write((i+2<96*96)?b64[b2&0x3F]:'=');
        if ((i/3%19)==18) Serial.println();
    }
    Serial.println("\n==SAVE_END==");
    Serial.printf("已发送 %s\n", label.c_str());
    }
    // 每10帧把眼部裁剪图传出来查看
    if (frameCount % 10 == 0) {
    Serial.println("==EYE_START==");
    for (int i = 0; i < 96*96; i += 3) {
        uint8_t b0=crop96[i];
        uint8_t b1=(i+1<96*96)?crop96[i+1]:0;
        uint8_t b2=(i+2<96*96)?crop96[i+2]:0;
        Serial.write(b64[b0>>2]);
        Serial.write(b64[((b0&3)<<4)|(b1>>4)]);
        Serial.write((i+1<96*96)?b64[((b1&0xF)<<2)|(b2>>6)]:'=');
        Serial.write((i+2<96*96)?b64[b2&0x3F]:'=');
        if ((i/3%19)==18) Serial.println();
    }
    Serial.println("\n==EYE_END==");
    }
    print_bar("eye_closed", conf_closed, EYE_CLOSED_THRESHOLD);
    print_bar("eye_open",   conf_open,   0.5f);

    // 嘴部推理
    float conf_c2=0, conf_o2=0, conf_yawn=0;
    Serial.println("[嘴部]");
    if (crop_to_gray96(MOUTH_X, MOUTH_Y, MOUTH_W, MOUTH_H))
        run_inference(conf_c2, conf_o2, conf_yawn);
    print_bar("yawning", conf_yawn, YAWN_THRESHOLD);

    // 眼睛状态
    bool eyeClosed = (conf_closed >= EYE_CLOSED_THRESHOLD);
    if (eyeClosed) closedCount++;

    // 哈欠验证
    uint32_t now = millis();
    if (conf_yawn >= YAWN_THRESHOLD) {
        if (!inYawn) { inYawn=true; yawnStartMs=now; }
        else if ((now-yawnStartMs) >= YAWN_DURATION_MS) {
            yawnTotal++; inYawn=false;
            Serial.println("  >>> 确认打哈欠！<<<");
        }
    } else inYawn=false;

    // PERCLOS + 疲劳等级
    float perclos = (float)closedCount/(float)frameCount*100.0f;
    Serial.printf("PERCLOS:%.1f%%  哈欠:%d次\n", perclos, yawnTotal);

    if      (perclos>PERCLOS_SEVERE || yawnTotal>=3)
        Serial.println(">>> [严重] SEVERE <<<");
    else if (perclos>PERCLOS_MILD   || yawnTotal>=1)
        Serial.println(">>> [轻度] MILD");
    else
        Serial.println(">>> 正常 NORMAL");
}

// ================================================================
//  串口命令处理
// ================================================================

static void handle_cmd() {
    if (!Serial.available()) return;
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd == "mode:calib") {
        mode = MODE_CALIB;
        camera_init(PIXFORMAT_JPEG);
        Serial.println("切换到标定模式");
    } else if (cmd == "mode:detect") {
        mode = MODE_DETECT;
        camera_init(PIXFORMAT_RGB565);
        frameCount=closedCount=yawnTotal=0; inYawn=false;
        Serial.println("切换到检测模式");
    } else if (cmd.startsWith("e:")) {
        sscanf(cmd.c_str()+2, "%d,%d,%d,%d",
               &EYE_X, &EYE_Y, &EYE_W, &EYE_H);
        Serial.printf("眼部框:(%d,%d) %dx%d\n", EYE_X,EYE_Y,EYE_W,EYE_H);
    } else if (cmd.startsWith("m:")) {
        sscanf(cmd.c_str()+2, "%d,%d,%d,%d",
               &MOUTH_X, &MOUTH_Y, &MOUTH_W, &MOUTH_H);
        Serial.printf("嘴部框:(%d,%d) %dx%d\n", MOUTH_X,MOUTH_Y,MOUTH_W,MOUTH_H);
    } else if (cmd == "save:open") {
        pending_save = "open";
        Serial.println("下一帧保存为 eye_open");
    } else if (cmd == "save:close") {
        pending_save = "close";
        Serial.println("下一帧保存为 eye_closed");
    } else if (cmd == "save:yawn") {
        pending_save = "yawn";
        Serial.println("下一帧保存为 yawning");
    } else if (cmd == "p") {
        running = !running;
        Serial.printf("推理%s\n", running?"继续":"暂停");
    } else if (cmd == "r") {
        frameCount=closedCount=yawnTotal=0; inYawn=false;
        Serial.println("统计已重置");
    } else if (cmd == "s") {
        Serial.printf("[MEM] RAM:%dKB PSRAM:%dKB\n",
            ESP.getFreeHeap()/1024, ESP.getFreePsram()/1024);
    } else if (cmd == "h") {
        Serial.println("mode:calib/detect  切换模式");
        Serial.println("e:x,y,w,h  调眼部框");
        Serial.println("m:x,y,w,h  调嘴部框");
        Serial.println("save:open/close/yawn  采集图片");
        Serial.println("p=暂停 r=重置 s=内存 h=帮助");
    }
}

// ================================================================
//  setup
// ================================================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n====================================");
    Serial.println("  驾盾 — 标定+疲劳检测合并版");
    Serial.println("====================================");
    Serial.printf("[MEM] RAM:%dKB PSRAM:%dKB\n",
        ESP.getFreeHeap()/1024, ESP.getPsramSize()/1024);

    // 默认标定模式，JPEG格式
    if (!camera_init(PIXFORMAT_JPEG)) while(1) delay(1000);

    Serial.print("[CAM] 预热");
    for (int i=0;i<5;i++) {
        camera_fb_t* w=esp_camera_fb_get();
        if (w) esp_camera_fb_return(w);
        Serial.print("."); delay(200);
    }
    Serial.println(" 完成");
    Serial.println("\n默认标定模式，打开 calibration_tool.html 连接串口");
    Serial.println("标定完成后发送 mode:detect 切换检测\n");
    Serial.println("发送 h 查看所有命令");
}

// ================================================================
//  loop
// ================================================================
void loop() {
    handle_cmd();
    if (!running) { delay(100); return; }

    if (mode == MODE_CALIB) {
        calib_send_frame();
        delay(800);
    } else {
        detect_loop();
        uint32_t t = millis();
        if (millis()-t < 500) delay(500-(millis()-t));
    }
}