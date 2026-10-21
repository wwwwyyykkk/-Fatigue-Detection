/**
 * cloud_face.cpp — 百度 AI 云端人脸识别
 *
 * 流程:
 *   1. 从 RGB565 帧裁剪人脸区域 → RGB888
 *   2. JPEG 编码 → Base64
 *   3. HTTPS POST 百度 face/v3/search
 *   4. 解析 JSON 响应 → driver_id + confidence
 *   5. 超时/失败返回 false, 调用方降级到本地 MobileFaceNet
 */

#include "cloud_face.h"
#include "base64.h"
#include "esp_jpeg_enc.h"  /* ESP official JPEG encoder */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "CLOUD";

#define BAIDU_TOKEN_URL  "https://aip.baidubce.com/oauth/2.0/token"
#define BAIDU_SEARCH_URL "https://aip.baidubce.com/rest/2.0/face/v3/search"
#define BAIDU_ADD_URL   "https://aip.baidubce.com/rest/2.0/face/v3/faceset/user/add"
#define FACE_SIZE         320    /* resize face to 320×320 before encoding */
#define NVS_NS           "baidu"
#define NVS_KEY_TOKEN    "access_token"
#define TOKEN_EXPIRE_SEC (29 * 24 * 3600)  /* 29 days, actual is 30 */

static char  s_access_token[256] = {0};
static char *s_api_key    = NULL;
static char *s_secret_key = NULL;

/* ── RGB565 pixel reader ─────────────────────────────── */
static inline void rgb565_to_rgb(uint16_t p, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
    *g = (uint8_t)(((p >>  5) & 0x3F) * 255 / 63);
    *b = (uint8_t)(( p        & 0x1F) * 255 / 31);
}

/* ── Crop + resize face region to FACE_SIZE×FACE_SIZE RGB888 ──
 * Strategy: make square = max(face_w, face_h) * 1.5, so face fills ~2/3 of image.
 * This ensures face >= 150px in output for Baidu's quality check. */
static uint8_t *crop_face_rgb888(const uint8_t *rgb565, int fw, int fh,
                                  const int box[4])
{
    int fx1 = box[0], fy1 = box[1], fx2 = box[2], fy2 = box[3];
    int cx = (fx1 + fx2) / 2, cy = (fy1 + fy2) / 2;
    int fw_ = fx2 - fx1, fh_ = fy2 - fy1;
    /* Square = longer face side * 2.0 (more context for Baidu face detector) */
    int side = (fw_ > fh_ ? fw_ : fh_) * 2;
    if (side < 32) side = 32;
    int half = side / 2;
    int x1 = cx - half, y1 = cy - half;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x1 + side > fw) x1 = fw - side;
    if (y1 + side > fh) y1 = fh - side;
    if (x1 < 0) { x1 = 0; side = fw; }
    if (y1 < 0) { y1 = 0; side = fh; }
    int rw = side, rh = side;
    ESP_LOGI(TAG, "Crop: face=%dx%d side=%d → out=%d (face≈%dpx)", fw_, fh_, side, FACE_SIZE,
             fh_ * FACE_SIZE / side);

    uint8_t *out = (uint8_t *)malloc(FACE_SIZE * FACE_SIZE * 3);
    if (!out) return NULL;

    for (int dy = 0; dy < FACE_SIZE; dy++) {
        float sy = (float)y1 + (float)dy / FACE_SIZE * rh;
        int siy = (int)sy, siy2 = siy + 1;
        if (siy2 >= fh) siy2 = fh - 1;
        float fy = sy - siy;

        for (int dx = 0; dx < FACE_SIZE; dx++) {
            float sx = (float)x1 + (float)dx / FACE_SIZE * rw;
            int six = (int)sx, six2 = six + 1;
            if (six2 >= fw) six2 = fw - 1;
            float fx = sx - six;

            uint8_t r00, g00, b00, r10, g10, b10;
            uint8_t r01, g01, b01, r11, g11, b11;
            rgb565_to_rgb(((uint16_t *)rgb565)[siy * fw + six],   &r00, &g00, &b00);
            rgb565_to_rgb(((uint16_t *)rgb565)[siy * fw + six2],  &r10, &g10, &b10);
            rgb565_to_rgb(((uint16_t *)rgb565)[siy2 * fw + six],  &r01, &g01, &b01);
            rgb565_to_rgb(((uint16_t *)rgb565)[siy2 * fw + six2], &r11, &g11, &b11);

            float r = (r00*(1-fx) + r10*fx)*(1-fy) + (r01*(1-fx) + r11*fx)*fy;
            float g = (g00*(1-fx) + g10*fx)*(1-fy) + (g01*(1-fx) + g11*fx)*fy;
            float b = (b00*(1-fx) + b10*fx)*(1-fy) + (b01*(1-fx) + b11*fx)*fy;

            int idx = (dy * FACE_SIZE + dx) * 3;
            out[idx] = (uint8_t)r; out[idx+1] = (uint8_t)g; out[idx+2] = (uint8_t)b;
        }
    }
    return out;
}

/* ── Get/Cache access_token ───────────────────────────── */
static bool _fetch_access_token(const char *api_key, const char *secret_key,
                                 char *token_out, size_t token_size)
{
    /* Check NVS cache first */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
        uint32_t saved_at = 0;
        nvs_get_u32(nvs, "saved_at", &saved_at);
        size_t len = token_size - 1;
        if (nvs_get_str(nvs, NVS_KEY_TOKEN, token_out, &len) == ESP_OK &&
            strlen(token_out) > 10) {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
            if (now - saved_at < TOKEN_EXPIRE_SEC) {
                ESP_LOGI(TAG, "Using cached access_token");
                nvs_close(nvs);
                return true;
            }
        }
        nvs_close(nvs);
    }

    /* Fetch new token from Baidu */
    char url[512];
    snprintf(url, sizeof(url),
             "%s?grant_type=client_credentials&client_id=%s&client_secret=%s",
             BAIDU_TOKEN_URL, api_key, secret_key);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 8000;
    cfg.buffer_size = 2048;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;  /* use built-in CA bundle for HTTPS */

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { ESP_LOGE(TAG, "HTTP init fail"); return false; }

    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) { ESP_LOGE(TAG, "HTTP open fail: %d", (int)err); esp_http_client_cleanup(client); return false; }
    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Token HTTP: status=%d content_len=%d", status, content_len);

    bool ok = false;
    if (content_len >= 0 || status == 200) {
        /* Allocate buffer: use content_len if known, else estimate */
        int buf_sz = content_len > 0 ? content_len + 1 : 2048;
        char *resp = (char *)malloc(buf_sz);
        if (resp) {
            int total = 0;
            while (total < buf_sz - 1) {
                int len = esp_http_client_read(client, resp + total, buf_sz - 1 - total);
                if (len <= 0) break;
                total += len;
            }
            if (total > 0) {
                resp[total] = '\0';
                ESP_LOGI(TAG, "Token body: %.200s", resp);
                cJSON *root = cJSON_Parse(resp);
                if (root) {
                    cJSON *tok = cJSON_GetObjectItem(root, "access_token");
                    if (tok && cJSON_IsString(tok)) {
                        strncpy(token_out, tok->valuestring, token_size - 1);
                        token_out[token_size - 1] = '\0';
                        /* Save to NVS */
                        nvs_handle_t nvs2;
                        if (nvs_open(NVS_NS, NVS_READWRITE, &nvs2) == ESP_OK) {
                            nvs_set_str(nvs2, NVS_KEY_TOKEN, token_out);
                            nvs_set_u32(nvs2, "saved_at",
                                        (uint32_t)(esp_timer_get_time() / 1000000));
                            nvs_commit(nvs2);
                            nvs_close(nvs2);
                        }
                        ok = true;
                        ESP_LOGI(TAG, "Got access_token: %.20s...", token_out);
                    }
                    cJSON_Delete(root);
                }
            }
            free(resp);
        }
    } else {
        ESP_LOGE(TAG, "Token fetch: status=%d err=%d", status, (int)err);
    }
    esp_http_client_cleanup(client);
    return ok;
}

/* ═══════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════ */

bool cloud_face_init(const char *api_key, const char *secret_key)
{
    if (!api_key || !secret_key ||
        strlen(api_key) < 5 || strlen(secret_key) < 5) {
        ESP_LOGE(TAG, "Invalid API credentials");
        return false;
    }

    s_api_key    = strdup(api_key);
    s_secret_key = strdup(secret_key);

    if (!s_api_key || !s_secret_key) {
        ESP_LOGE(TAG, "OOM storing credentials"); return false;
    }

    bool ok = _fetch_access_token(s_api_key, s_secret_key,
                                   s_access_token, sizeof(s_access_token));
    if (ok)
        ESP_LOGI(TAG, "cloud_face_init OK");
    else
        ESP_LOGE(TAG, "cloud_face_init FAILED — cloud recognition unavailable");
    return ok;
}

bool cloud_face_identify(
    const uint8_t *frame_rgb565, int frame_w, int frame_h,
    const int face_box[4],
    int *driver_id, float *confidence,
    int timeout_ms)
{
    *driver_id = -1;
    *confidence = 0.0f;

    if (!s_access_token[0]) {
        if (!_fetch_access_token(s_api_key, s_secret_key,
                                  s_access_token, sizeof(s_access_token)))
            return false;
    }

    int64_t t0 = esp_timer_get_time();

    /* 1. Crop face → RGB888 (square, face fills ~2/3 of image) */
    uint8_t *face_rgb = crop_face_rgb888(frame_rgb565, frame_w, frame_h, face_box);
    if (!face_rgb) return false;

    /* 2. JPEG encode with ESP official encoder (esp_new_jpeg) */
    jpeg_enc_config_t enc_config = {
        .width = FACE_SIZE,
        .height = FACE_SIZE,
        .src_type = JPEG_PIXEL_FORMAT_RGB888,
        .subsampling = JPEG_SUBSAMPLE_420,
        .quality = 95,
        .rotate = JPEG_ROTATE_0D,
        .task_enable = false,
        .hfm_task_priority = 0,
        .hfm_task_core = 0,
    };

    jpeg_enc_handle_t jpeg_enc = NULL;
    jpeg_error_t ret = jpeg_enc_open(&enc_config, &jpeg_enc);
    if (ret != JPEG_ERR_OK || !jpeg_enc) {
        ESP_LOGE(TAG, "JPEG encoder open fail: %d", ret);
        free(face_rgb);
        return false;
    }

    /* Allocate output buffer (must be large enough for worst case) */
    int jpg_buf_size = FACE_SIZE * FACE_SIZE * 3 / 2 + 4096;  /* 150% + header margin */
    uint8_t *jpg_buf = (uint8_t *)malloc(jpg_buf_size);
    if (!jpg_buf) {
        jpeg_enc_close(jpeg_enc);
        free(face_rgb);
        return false;
    }

    int jpg_len = 0;
    ret = jpeg_enc_process(jpeg_enc, face_rgb, FACE_SIZE * FACE_SIZE * 3,
                           jpg_buf, jpg_buf_size, &jpg_len);
    jpeg_enc_close(jpeg_enc);
    free(face_rgb);

    if (ret != JPEG_ERR_OK || jpg_len == 0) {
        ESP_LOGE(TAG, "JPEG encode fail: %d", ret);
        free(jpg_buf);
        return false;
    }

    /* DEBUG: dump first 200 bytes of JPEG to verify header */
    ESP_LOGI(TAG, "JPEG encoded: %d bytes", jpg_len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, jpg_buf, jpg_len < 200 ? jpg_len : 200, ESP_LOG_INFO);

    /* 3. Base64 encode */
    int b64_size = ((jpg_len + 2) / 3) * 4 + 10;
    char *b64 = (char *)malloc(b64_size);
    if (!b64) { free(jpg_buf); return false; }
    int b64_len = base64_encode(jpg_buf, (size_t)jpg_len, b64, b64_size);
    free(jpg_buf);
    if (b64_len < 0) { free(b64); return false; }
    b64[b64_len] = '\0';

    /* 4. Build JSON body */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "image", b64);
    cJSON_AddStringToObject(body, "image_type", "BASE64");
    cJSON_AddStringToObject(body, "group_id_list", "drivers");
    cJSON_AddStringToObject(body, "quality_control", "NONE");
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    free(b64);

    if (!body_str) return false;

    /* 5. HTTP POST */
    char url[512];
    snprintf(url, sizeof(url), "%s?access_token=%s",
             BAIDU_SEARCH_URL, s_access_token);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = timeout_ms > 0 ? timeout_ms : 3000;
    cfg.buffer_size = 4096;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;  /* use built-in CA bundle for HTTPS */

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(body_str); return false; }

    esp_http_client_set_header(client, "Content-Type", "application/json");

    int body_len = (int)strlen(body_str);
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Search HTTP open fail"); free(body_str); esp_http_client_cleanup(client); return false; }
    /* Manually write POST body — required with open/write/fetch/read pattern */
    int written = esp_http_client_write(client, body_str, body_len);
    ESP_LOGI(TAG, "Search write: %d/%d bytes", written, body_len);
    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Search HTTP: status=%d content_len=%d", status, content_len);

    bool ok = false;
    if (status == 200 || status == 201) {
        int buf_sz = content_len > 0 ? content_len + 1 : 4096;
        char *resp = (char *)malloc(buf_sz);
        if (resp) {
            int total = 0;
            while (total < buf_sz - 1) {
                int len = esp_http_client_read(client, resp + total, buf_sz - 1 - total);
                if (len <= 0) break;
                total += len;
            }
            if (total > 0) {
                resp[total] = '\0';
                cJSON *root = cJSON_Parse(resp);
                if (root) {
                    cJSON *errcode = cJSON_GetObjectItem(root, "error_code");
                    int api_err = errcode && cJSON_IsNumber(errcode) ? errcode->valueint : -999;
                    ESP_LOGI(TAG, "Search resp: errcode=%d body=%.300s", api_err, resp);
                    cJSON *result  = cJSON_GetObjectItem(root, "result");
                    cJSON *ulist   = result ? cJSON_GetObjectItem(result, "user_list") : NULL;
                    int n_users = ulist ? cJSON_GetArraySize(ulist) : 0;
                    ESP_LOGI(TAG, "Search users: %d", n_users);

                    if (api_err != 0) {
                        /* Baidu error (image check fail etc.) → fallback to local */
                        ESP_LOGW(TAG, "Baidu errcode=%d, fallback to local", api_err);
                        ok = false;
                    } else if (n_users > 0) {
                        cJSON *user  = cJSON_GetArrayItem(ulist, 0);
                        cJSON *uid   = cJSON_GetObjectItem(user, "user_id");
                        cJSON *score = cJSON_GetObjectItem(user, "score");
                        const char *uid_str = uid && cJSON_IsString(uid) ? uid->valuestring : "?";
                        float s = score && cJSON_IsNumber(score) ? (float)score->valuedouble : 0.0f;
                        ESP_LOGI(TAG, "Search #0: uid=%s score=%.1f", uid_str, s);
                        if (uid && cJSON_IsString(uid) && score && cJSON_IsNumber(score)) {
                            int did = -1;
                            sscanf(uid->valuestring, "driver_%d", &did);
                            if (did > 0) {
                                *driver_id = did;
                                *confidence = s;
                                ok = true;
                            } else {
                                /* matched but user_id not in our format */
                                *driver_id = -1;
                                *confidence = s;
                                ok = true;
                            }
                        }
                    } else {
                        /* No match — but API succeeded (face is unknown) */
                        *driver_id = -1;
                        *confidence = 0.0f;
                        ok = true;  /* cloud worked, just no match */
                    }
                    cJSON_Delete(root);
                }
            }
            free(resp);
        }
    } else {
        ESP_LOGW(TAG, "Search API: status=%d err=%d", status, (int)err);
    }

    esp_http_client_cleanup(client);
    free(body_str);

    int64_t elapsed = (esp_timer_get_time() - t0) / 1000;
    if (ok) {
        ESP_LOGI(TAG, "identify OK: driver=%d conf=%.1f%% time=%lldms",
                 *driver_id, *confidence, elapsed);
    }
    return ok;
}

bool cloud_face_enroll(
    const uint8_t *frame_rgb565, int frame_w, int frame_h,
    const int face_box[4],
    int driver_id, int timeout_ms)
{
    if (!s_access_token[0] || driver_id < 1) return false;

    int64_t t0 = esp_timer_get_time();

    /* 1. Crop face → RGB888 */
    uint8_t *face_rgb = crop_face_rgb888(frame_rgb565, frame_w, frame_h, face_box);
    if (!face_rgb) return false;

    /* 2. JPEG encode with ESP official encoder (esp_new_jpeg) */
    jpeg_enc_config_t enc_config = {
        .width = FACE_SIZE,
        .height = FACE_SIZE,
        .src_type = JPEG_PIXEL_FORMAT_RGB888,
        .subsampling = JPEG_SUBSAMPLE_420,
        .quality = 90,
        .rotate = JPEG_ROTATE_0D,
        .task_enable = false,
        .hfm_task_priority = 0,
        .hfm_task_core = 0,
    };

    jpeg_enc_handle_t jpeg_enc = NULL;
    jpeg_error_t ret = jpeg_enc_open(&enc_config, &jpeg_enc);
    if (ret != JPEG_ERR_OK || !jpeg_enc) {
        ESP_LOGE(TAG, "JPEG encoder open fail: %d", ret);
        free(face_rgb);
        return false;
    }

    int jpg_buf_size = FACE_SIZE * FACE_SIZE * 3 / 2 + 4096;  /* 150% + header margin */
    uint8_t *jpg_buf = (uint8_t *)malloc(jpg_buf_size);
    if (!jpg_buf) {
        jpeg_enc_close(jpeg_enc);
        free(face_rgb);
        return false;
    }

    int jpg_len = 0;
    ret = jpeg_enc_process(jpeg_enc, face_rgb, FACE_SIZE * FACE_SIZE * 3,
                           jpg_buf, jpg_buf_size, &jpg_len);
    jpeg_enc_close(jpeg_enc);
    free(face_rgb);

    if (ret != JPEG_ERR_OK || jpg_len == 0) {
        ESP_LOGE(TAG, "JPEG encode fail: %d", ret);
        free(jpg_buf);
        return false;
    }

    /* 3. Base64 encode */
    int b64_size = ((jpg_len + 2) / 3) * 4 + 10;
    char *b64 = (char *)malloc(b64_size);
    if (!b64) { free(jpg_buf); return false; }
    int b64_len = base64_encode(jpg_buf, (size_t)jpg_len, b64, b64_size);
    free(jpg_buf);
    if (b64_len < 0) { free(b64); return false; }
    b64[b64_len] = '\0';

    /* 4. Build JSON body */
    char uid_str[32];
    snprintf(uid_str, sizeof(uid_str), "driver_%d", driver_id);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "image", b64);
    cJSON_AddStringToObject(body, "image_type", "BASE64");
    cJSON_AddStringToObject(body, "group_id", "drivers");
    cJSON_AddStringToObject(body, "user_id", uid_str);
    cJSON_AddStringToObject(body, "action_type", "REPLACE");
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    free(b64);
    if (!body_str) return false;

    /* 5. HTTP POST */
    char url[512];
    snprintf(url, sizeof(url), "%s?access_token=%s",
             BAIDU_ADD_URL, s_access_token);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = timeout_ms > 0 ? timeout_ms : 8000;
    cfg.buffer_size = 4096;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(body_str); return false; }

    esp_http_client_set_header(client, "Content-Type", "application/json");

    int body_len = (int)strlen(body_str);
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Enroll HTTP open fail"); free(body_str); esp_http_client_cleanup(client); return false; }
    int written = esp_http_client_write(client, body_str, body_len);
    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    bool ok = false;
    if (status == 200) {
        int buf_sz = content_len > 0 ? content_len + 1 : 1024;
        char *resp = (char *)malloc(buf_sz);
        if (resp) {
            int total = 0;
            while (total < buf_sz - 1) {
                int len = esp_http_client_read(client, resp + total, buf_sz - 1 - total);
                if (len <= 0) break;
                total += len;
            }
            if (total > 0) {
                resp[total] = '\0';
                cJSON *root = cJSON_Parse(resp);
                if (root) {
                    cJSON *code = cJSON_GetObjectItem(root, "error_code");
                    if (code && cJSON_IsNumber(code) && code->valueint == 0) {
                        ok = true;
                        ESP_LOGI(TAG, "enroll OK: %s → cloud", uid_str);
                    } else {
                        cJSON *msg = cJSON_GetObjectItem(root, "error_msg");
                        ESP_LOGW(TAG, "enroll fail: %s", msg && cJSON_IsString(msg) ? msg->valuestring : "?");
                    }
                    cJSON_Delete(root);
                }
            }
            free(resp);
        }
    } else {
        ESP_LOGW(TAG, "Enroll API: status=%d err=%d", status, (int)err);
    }

    esp_http_client_cleanup(client);
    free(body_str);

    int64_t elapsed = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "enroll %s: %s (%lldms)", uid_str, ok ? "OK" : "FAIL", elapsed);
    return ok;
}

void cloud_face_deinit(void)
{
    if (s_api_key)    { free(s_api_key); s_api_key = NULL; }
    if (s_secret_key) { free(s_secret_key); s_secret_key = NULL; }
    memset(s_access_token, 0, sizeof(s_access_token));
}
