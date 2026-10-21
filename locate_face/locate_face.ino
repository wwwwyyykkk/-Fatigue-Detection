/*
 * 裁剪位置标定程序
 * 通过串口实时传输画面，网页上拖动滑块调整裁剪框
 * 找到准确位置后把比例值记下来
 */

#include "esp_camera.h"

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

// 当前裁剪比例（串口调整）
float EYE_X_R   = 124.0f / 320.0f;  // = 0.388
float EYE_Y_R   =  52.0f / 240.0f;  // = 0.217
float EYE_W_R   =  73.0f / 320.0f;  // = 0.228
float EYE_H_R   =  30.0f / 240.0f;  // = 0.125

float MTH_X_R = 131.0f / 320.0f;  // = 0.409
float MTH_Y_R =  83.0f / 240.0f;  // = 0.346
float MTH_W_R =  58.0f / 320.0f;  // = 0.181
float MTH_H_R =  35.0f / 240.0f;  // = 0.146
static camera_fb_t* fb = NULL;

static bool camera_init() {
    camera_config_t cfg;
    cfg.ledc_channel=LEDC_CHANNEL_0; cfg.ledc_timer=LEDC_TIMER_0;
    cfg.pin_pwdn=CAM_PWDN; cfg.pin_reset=CAM_RESET;
    cfg.pin_xclk=CAM_XCLK;
    cfg.pin_sccb_sda=CAM_SIOD; cfg.pin_sccb_scl=CAM_SIOC;
    cfg.pin_d7=CAM_Y7; cfg.pin_d6=CAM_Y6; cfg.pin_d5=CAM_Y5;
    cfg.pin_d4=CAM_Y4; cfg.pin_d3=CAM_Y3; cfg.pin_d2=CAM_Y2;
    cfg.pin_d1=CAM_Y1; cfg.pin_d0=CAM_Y0;
    cfg.pin_vsync=CAM_VSYNC; cfg.pin_href=CAM_HREF; cfg.pin_pclk=CAM_PCLK;
    cfg.xclk_freq_hz=20000000;
    cfg.pixel_format=PIXFORMAT_JPEG;  // JPEG方便网页显示
    cfg.frame_size=FRAMESIZE_QVGA;
    cfg.jpeg_quality=12;
    cfg.fb_count=1;
    cfg.grab_mode=CAMERA_GRAB_WHEN_EMPTY;
    cfg.fb_location=CAMERA_FB_IN_DRAM;

    pinMode(CAM_PWDN,OUTPUT); pinMode(CAM_RESET,OUTPUT);
    digitalWrite(CAM_PWDN,HIGH); delay(100);
    digitalWrite(CAM_RESET,LOW); delay(100);
    digitalWrite(CAM_PWDN,LOW);  delay(100);
    digitalWrite(CAM_RESET,HIGH); delay(200);

    return esp_camera_init(&cfg) == ESP_OK;
}

// Base64 发送 JPEG
void send_jpeg() {
    if (fb) { esp_camera_fb_return(fb); fb=NULL; }
    fb = esp_camera_fb_get();
    if (!fb) return;

    // 发送裁剪框参数
    Serial.printf("##PARAMS:%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f##\n",
        EYE_X_R,EYE_Y_R,EYE_W_R,EYE_H_R,
        MTH_X_R,MTH_Y_R,MTH_W_R,MTH_H_R);

    // Base64 发送图片
    const char b64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    Serial.println("==IMG_START==");
    uint8_t* d=fb->buf; size_t len=fb->len;
    for (size_t i=0;i<len;i+=3) {
        uint8_t b0=d[i],b1=(i+1<len)?d[i+1]:0,b2=(i+2<len)?d[i+2]:0;
        Serial.write(b64[b0>>2]);
        Serial.write(b64[((b0&3)<<4)|(b1>>4)]);
        Serial.write((i+1<len)?b64[((b1&0xF)<<2)|(b2>>6)]:'=');
        Serial.write((i+2<len)?b64[b2&0x3F]:'=');
        if ((i/3%19)==18) Serial.println();
    }
    Serial.println("\n==IMG_END==");
}

void handle_cmd() {
    if (!Serial.available()) return;
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.startsWith("e:"))
        sscanf(cmd.c_str()+2,"%f,%f,%f,%f",&EYE_X_R,&EYE_Y_R,&EYE_W_R,&EYE_H_R);
    else if (cmd.startsWith("m:"))
        sscanf(cmd.c_str()+2,"%f,%f,%f,%f",&MTH_X_R,&MTH_Y_R,&MTH_W_R,&MTH_H_R);
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("标定程序启动");
    if (!camera_init()) { Serial.println("摄像头失败"); while(1) delay(1000); }
    Serial.println("就绪，打开 calibration_tool.html 连接串口");
}

void loop() {
    handle_cmd();
    send_jpeg();
    delay(800);  // 约1fps，够标定用
}