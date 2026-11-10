/**
 * @file app_web_eyes.c
 * @brief 眼睛运行时控制 REST API（仅接口，不包含页面）
 *
 * 路由（Port 80，经 ctrl_dispatch 软件分发）：
 *   GET  /api/eyes/meta            — 能力/当前状态（供网页与 eyes-studio 发现）
 *   POST /api/eyes/emotion         — {v:0..16}              应用内置表情
 *   POST /api/eyes/preset          — {v:{...23 个 preset 字段...}} 应用整套表情（运行时，不落盘）
 *   POST /api/eyes/lid             — {eye,lid,upper,lower,ms}  单眼眼睑（形状+开度）
 *   POST /api/eyes/position        — {eye?,x,y,ms}            视线（eye 缺省=双眼）
 *   POST /api/eyes/pupil           — {eye?,v,ms}              瞳孔（eye 缺省=双眼）
 *   POST /api/eyes/scale           — {eye?,v,ms}              虹膜缩放（eye 缺省=双眼）
 *   POST /api/eyes/iris            — {v:0..7}                内置虹膜纹理
 *   POST /api/eyes/blink           — {v:ms}                  双眼眨眼
 *   POST /api/eyes/wink            — {eye:0|1}               单眼眨眼
 *
 * preset 字段名与 Little Eyes Studio / eyes_bridge 协议一致：
 *   leftLids, rightLids, leftUpper, leftLower, rightUpper, rightLower,
 *   leftScale, rightScale, pupil, leftPosX, leftPosY, rightPosX, rightPosY,
 *   easePosMs, easeScaleMs, easePupilMs, displayMode,
 *   autoMove, autoBlink, autoPupils, autoAlive,
 *   disableUpperJitter, disableLowerJitter
 *
 * 说明：
 *   - 仅"运行时生效"，不落盘（重启还原）；
 *   - 应用 preset / 单眼直调 前会调用 logic_emotion_hold_custom()，
 *     推迟 ~8~15s 后的自动随机切换，便于实时预览；
 *   - 与既有 remote_protocol / xbox 一样，在 httpd 线程直接调 eyes_control，
 *     库侧已用 eyes 临界区保护关键状态写。
 */

#include "app_webserver.h"
#include "eyes_control.h"
#include "eyes_core.h"

#include <string.h>
#include <cJSON.h>
#include "esp_log.h"
#include "esp_http_server.h"

#define TAG "WEB_EYES"

/* 单个请求 body 上限（preset JSON 约 <700B，留余量；也限制 handler 栈占用） */
#define EYES_BODY_MAX  1024

/* ================================================================
 *  JSON / 响应小工具
 * ================================================================ */
static const cJSON *jget(const cJSON *o, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(o, key);
}

static int jint(const cJSON *o, const char *key, int dflt)
{
    const cJSON *v = jget(o, key);
    if (cJSON_IsNumber(v)) return v->valuedouble >= 0 ? (int)v->valuedouble : (int)(v->valuedouble - 0.5);
    if (cJSON_IsBool(v))   return cJSON_IsTrue(v) ? 1 : 0;
    return dflt;
}

static double jnum(const cJSON *o, const char *key, double dflt)
{
    const cJSON *v = jget(o, key);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsBool(v))   return cJSON_IsTrue(v) ? 1.0 : 0.0;
    return dflt;
}

static bool jbool(const cJSON *o, const char *key, bool dflt)
{
    const cJSON *v = jget(o, key);
    if (cJSON_IsBool(v))   return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    return dflt;
}

static esp_err_t send_eyes_ok(httpd_req_t *req, const char *extra)
{
    char buf[256];
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    int len;
    if (extra) {
        len = snprintf(buf, sizeof(buf), "{\"status\":\"ok\",%s}", extra);
    } else {
        len = snprintf(buf, sizeof(buf), "{\"status\":\"ok\"}");
    }
    return httpd_resp_send(req, buf, len);
}

static esp_err_t send_eyes_err(httpd_req_t *req, const char *msg)
{
    char buf[200];
    int len = snprintf(buf, sizeof(buf), "{\"status\":\"error\",\"error\":\"%s\"}", msg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, buf, len);
}

/* 读完整 body（httpd 单线程串行分发，静态缓冲可复用；本模块 body 很小） */
static char *recv_body(httpd_req_t *req, char *buf, size_t cap)
{
    size_t total = 0;
    if (req->content_len == 0) return NULL;
    if (req->content_len >= cap) return NULL;
    while (total < req->content_len) {
        int got = httpd_req_recv(req, buf + total, req->content_len - total);
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return NULL;
        }
        total += (size_t)got;
    }
    buf[total] = '\0';
    return buf;
}

/* 兼容 eyes-studio：返回目标眼睛数组。eye<0/缺省 => 双眼 */
static bool eye_of(const cJSON *root, int *eye_out, bool *is_both)
{
    int eye = jint(root, "eye", -1);
    *is_both = (eye < 0 || eye > 1);
    *eye_out = (eye >= 0 && eye <= 1) ? eye : 0;
    return true;
}

/* ================================================================
 *  GET /api/eyes/meta
 * ================================================================ */
static esp_err_t eyes_meta_handler(httpd_req_t *req)
{
    EyeController *ctl = eye_controller_get_instance();
    char buf[384];
    int len = snprintf(buf, sizeof(buf),
        "{\"status\":\"ok\","
        "\"emotions\":%d,\"lids\":%d,\"irises\":%d,"
        "\"emotion\":%d,\"manual\":%s}",
        EYES_EMOTION_COUNT, EYES_LID_COUNT, EYES_IRIS_COUNT,
        (ctl ? ctl->eye_emotion_type : -1),
        eye_controller_is_manual_emotion() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, buf, len);
}

/* ================================================================
 *  POST /api/eyes/emotion  {v:0..16}
 * ================================================================ */
static esp_err_t eyes_emotion_handler(httpd_req_t *req)
{
    char body[EYES_BODY_MAX];
    if (!recv_body(req, body, sizeof(body))) return send_eyes_err(req, "bad body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_eyes_err(req, "bad json");

    int v = jint(root, "v", -1);
    cJSON_Delete(root);
    if (v < 0 || v >= EYES_EMOTION_COUNT) return send_eyes_err(req, "bad emotion");

    eye_controller_apply_emotion((EYES_EMOTION)v);
    ESP_LOGI(TAG, "emotion -> %d", v);
    return send_eyes_ok(req, NULL);
}

/* ================================================================
 *  POST /api/eyes/preset  {v:{...}}
 * ================================================================ */
static esp_err_t eyes_preset_handler(httpd_req_t *req)
{
    char body[EYES_BODY_MAX];
    if (!recv_body(req, body, sizeof(body))) return send_eyes_err(req, "bad body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_eyes_err(req, "bad json");

    const cJSON *v = jget(root, "v");
    if (!cJSON_IsObject(v)) {
        cJSON_Delete(root);
        return send_eyes_err(req, "missing v{}");
    }

    EyeExpressionPreset p;
    memset(&p, 0, sizeof(p));

    p.left_lids   = (EYES_LIDS_DEFINE)jint(v, "leftLids", 0);
    p.right_lids  = (EYES_LIDS_DEFINE)jint(v, "rightLids", 0);
    p.left_upper  = (float)jnum(v, "leftUpper", 1.0f);
    p.left_lower  = (float)jnum(v, "leftLower", 1.0f);
    p.right_upper = (float)jnum(v, "rightUpper", 1.0f);
    p.right_lower = (float)jnum(v, "rightLower", 1.0f);
    p.left_scale  = (float)jnum(v, "leftScale", 1.0f);
    p.right_scale = (float)jnum(v, "rightScale", 1.0f);
    p.pupil       = (float)jnum(v, "pupil", 0.65f);
    p.left_posX   = (float)jnum(v, "leftPosX", 0.0f);
    p.left_posY   = (float)jnum(v, "leftPosY", 0.0f);
    p.right_posX  = (float)jnum(v, "rightPosX", 0.0f);
    p.right_posY  = (float)jnum(v, "rightPosY", 0.0f);
    p.easePosMs   = (uint16_t)jint(v, "easePosMs", 400);
    p.easeScaleMs = (uint16_t)jint(v, "easeScaleMs", 400);
    p.easePupilMs = (uint16_t)jint(v, "easePupilMs", 800);
    p.displayMode = (uint8_t)jint(v, "displayMode", 0);
    p.autoMove    = jbool(v, "autoMove", false);
    p.autoBlink   = jbool(v, "autoBlink", false);
    p.autoPupils  = jbool(v, "autoPupils", false);
    p.autoAlive   = jbool(v, "autoAlive", false);
    p.disableUpperJitter = jbool(v, "disableUpperJitter", false);
    p.disableLowerJitter = jbool(v, "disableLowerJitter", false);

    cJSON_Delete(root);

    if (p.left_lids >= EYES_LID_COUNT) p.left_lids = EYES_LID_CLASSIC;
    if (p.right_lids >= EYES_LID_COUNT) p.right_lids = EYES_LID_CLASSIC;

    eyes_control_apply_preset(&p);
    ESP_LOGI(TAG, "preset applied (lids=%d/%d up=%.2f/%.2f)",
             p.left_lids, p.right_lids, p.left_upper, p.right_upper);
    return send_eyes_ok(req, NULL);
}

/* ================================================================
 *  POST /api/eyes/lid  {eye,lid,upper,lower,ms}
 * ================================================================ */
static esp_err_t eyes_lid_handler(httpd_req_t *req)
{
    char body[EYES_BODY_MAX];
    if (!recv_body(req, body, sizeof(body))) return send_eyes_err(req, "bad body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_eyes_err(req, "bad json");

    int eye = jint(root, "eye", -1);
    int lid = jint(root, "lid", -1);
    if (eye < 0 || eye > 1 || lid < 0 || lid >= EYES_LID_COUNT) {
        cJSON_Delete(root);
        return send_eyes_err(req, "bad eye/lid");
    }
    float up  = (float)jnum(root, "upper", 1.0);
    float low = (float)jnum(root, "lower", 1.0);
    int ms    = jint(root, "ms", 0);
    cJSON_Delete(root);

    eyes_control_set_lid((uint32_t)eye, (EYES_LIDS_DEFINE)lid, up, low, (uint32_t)ms);
    ESP_LOGI(TAG, "lid eye=%d type=%d up=%.2f low=%.2f ms=%d", eye, lid, up, low, ms);
    return send_eyes_ok(req, NULL);
}

/* ================================================================
 *  POST /api/eyes/position / pupil / scale
 * ================================================================ */
static esp_err_t eyes_position_handler(httpd_req_t *req)
{
    char body[EYES_BODY_MAX];
    if (!recv_body(req, body, sizeof(body))) return send_eyes_err(req, "bad body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_eyes_err(req, "bad json");

    bool both; int eye;
    eye_of(root, &eye, &both);
    float x = (float)jnum(root, "x", 0.0);
    float y = (float)jnum(root, "y", 0.0);
    int ms = jint(root, "ms", 300);
    cJSON_Delete(root);

    if (both) {
        eyes_control_gaze(0, x, y, ms);
        eyes_control_gaze(1, x, y, ms);
    } else {
        eyes_control_gaze((uint32_t)eye, x, y, ms);
    }
    return send_eyes_ok(req, NULL);
}

static esp_err_t eyes_pupil_handler(httpd_req_t *req)
{
    char body[EYES_BODY_MAX];
    if (!recv_body(req, body, sizeof(body))) return send_eyes_err(req, "bad body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_eyes_err(req, "bad json");

    bool both; int eye;
    eye_of(root, &eye, &both);
    float v  = (float)jnum(root, "v", 0.65);
    int   ms = jint(root, "ms", 400);
    cJSON_Delete(root);

    if (both) {
        eyes_control_pupil(0, v, ms);
        eyes_control_pupil(1, v, ms);
    } else {
        eyes_control_pupil((uint32_t)eye, v, ms);
    }
    return send_eyes_ok(req, NULL);
}

static esp_err_t eyes_scale_handler(httpd_req_t *req)
{
    char body[EYES_BODY_MAX];
    if (!recv_body(req, body, sizeof(body))) return send_eyes_err(req, "bad body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_eyes_err(req, "bad json");

    bool both; int eye;
    eye_of(root, &eye, &both);
    float v  = (float)jnum(root, "v", 1.0);
    int   ms = jint(root, "ms", 400);
    cJSON_Delete(root);

    if (both) {
        eyes_control_scale(0, v, ms);
        eyes_control_scale(1, v, ms);
    } else {
        eyes_control_scale((uint32_t)eye, v, ms);
    }
    return send_eyes_ok(req, NULL);
}

/* ================================================================
 *  POST /api/eyes/iris  {v:0..7}
 * ================================================================ */
static esp_err_t eyes_iris_handler(httpd_req_t *req)
{
    char body[EYES_BODY_MAX];
    if (!recv_body(req, body, sizeof(body))) return send_eyes_err(req, "bad body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_eyes_err(req, "bad json");

    int v = jint(root, "v", -1);
    cJSON_Delete(root);
    if (v < 0 || v >= EYES_IRIS_COUNT) return send_eyes_err(req, "bad iris");

    eyes_enter_critical();
    eye_controller_apply_iris_texture((EYES_IRIS_DEFINE)v);
    eyes_exit_critical();
    ESP_LOGI(TAG, "iris -> %d", v);
    return send_eyes_ok(req, NULL);
}

/* ================================================================
 *  POST /api/eyes/blink {v:ms} / wink {eye}
 * ================================================================ */
static esp_err_t eyes_blink_handler(httpd_req_t *req)
{
    char body[EYES_BODY_MAX];
    if (!recv_body(req, body, sizeof(body))) return send_eyes_err(req, "bad body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_eyes_err(req, "bad json");

    int ms = jint(root, "v", 230);
    cJSON_Delete(root);

    eyes_control_blink((uint32_t)ms);
    return send_eyes_ok(req, NULL);
}

static esp_err_t eyes_wink_handler(httpd_req_t *req)
{
    char body[EYES_BODY_MAX];
    if (!recv_body(req, body, sizeof(body))) return send_eyes_err(req, "bad body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_eyes_err(req, "bad json");

    int eye = jint(root, "eye", -1);
    cJSON_Delete(root);
    if (eye < 0 || eye > 1) return send_eyes_err(req, "bad eye");

    eyes_control_wink((uint32_t)eye);
    return send_eyes_ok(req, NULL);
}

/* ================================================================
 *  路由表（app_webserver.c 在 httpd 启动前调用本模块 init）
 * ================================================================ */
static const web_route_t s_routes[] = {
    { "/api/eyes/meta",     HTTP_GET,  eyes_meta_handler,     false },
    { "/api/eyes/emotion",  HTTP_POST, eyes_emotion_handler,  false },
    { "/api/eyes/preset",   HTTP_POST, eyes_preset_handler,   false },
    { "/api/eyes/lid",      HTTP_POST, eyes_lid_handler,      false },
    { "/api/eyes/position", HTTP_POST, eyes_position_handler, false },
    { "/api/eyes/pupil",    HTTP_POST, eyes_pupil_handler,    false },
    { "/api/eyes/scale",    HTTP_POST, eyes_scale_handler,    false },
    { "/api/eyes/iris",     HTTP_POST, eyes_iris_handler,     false },
    { "/api/eyes/blink",    HTTP_POST, eyes_blink_handler,    false },
    { "/api/eyes/wink",     HTTP_POST, eyes_wink_handler,     false },
};

void app_web_eyes_init(void)
{
    app_web_register_routes(WEB_SERVER_CTRL, s_routes,
                            sizeof(s_routes) / sizeof(s_routes[0]));
    ESP_LOGI(TAG, "%d eyes API routes registered",
             (int)(sizeof(s_routes) / sizeof(s_routes[0])));
}
