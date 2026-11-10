/**
 * @file app_web_settings.c
 * @brief 设置页面 / REST 配置 API
 *
 * 负责路由:
 *   GET  /settings   — 返回 settings.html
 *   POST /api/config — 统一配置 API (JSON body)
 *
 * API action 列表:
 *   get           — 读取全部配置（bluetooth、ota_url、eye_iris、wifi_ssid）
 *   set_bluetooth — 开关 BLE
 *   set_ota_url   — 设置 OTA 地址
 *   set_eye_iris  — 设置眼睛虹膜参数
 *   set_wifi      — 设置 Wi-Fi 凭证并重启
 *   set_servo_trim  — 保存舵机校准偏移并立即生效
 *   set_servo_angle — 舵机测试：直接下发双舵机逻辑角度
 *
 * 扩展方式:
 *   新增 action 只需实现一个 action_fn_t 函数，然后在 s_actions[] 末尾追加一行。
 */

#include "app_webserver.h"
#include "dooi_nvs.h"
#include "app_ble_control.h"
#include "app_periphral.h"
#include "app_ui.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"

#define TAG "WEB_SETTINGS"

/* ---- 嵌入 HTML ---- */
extern const uint8_t settings_html_start[] asm("_binary_settings_html_start");
extern const uint8_t settings_html_end[]   asm("_binary_settings_html_end");

/* ================================================================
 *  JSON 解析辅助（轻量实现，无需第三方库）
 * ================================================================ */
static bool json_get_string(const char *json, const char *key,
                             char *out, size_t out_size)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *start = strstr(json, search);
    if (!start) return false;
    start += strlen(search);
    const char *end = strchr(start, '"');
    if (!end) return false;
    size_t len = (size_t)(end - start);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

static bool json_get_int(const char *json, const char *key, int *out)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *start = strstr(json, search);
    if (!start) return false;
    start += strlen(search);
    while (*start == ' ') start++;
    *out = atoi(start);
    return true;
}

static bool json_get_bool(const char *json, const char *key, bool *out)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *start = strstr(json, search);
    if (!start) return false;
    start += strlen(search);
    while (*start == ' ') start++;
    *out = (strncmp(start, "true", 4) == 0);
    return true;
}

/* ================================================================
 *  通用 JSON 应答辅助
 * ================================================================ */
static void send_json_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "{\"status\":\"ok\"}", 15);
}

/* ================================================================
 *  Handler: GET /settings
 * ================================================================ */
static esp_err_t settings_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req,
        (const char *)settings_html_start,
        (ssize_t)(settings_html_end - settings_html_start));
}

/* ================================================================
 *  action 处理函数（每个 action 独立，便于维护和扩展）
 * ================================================================ */

/* action: get — 读取并返回全部配置 */
static esp_err_t action_get(httpd_req_t *req, const char *body)
{
    (void)body;

    char    ota_url[128]  = {0};
    char    wifi_ssid[32] = {0};
    uint8_t eye_iris      = 0;
    bool    mot_l=false, mot_r=false, srv_l=false, srv_r=false;
    float   trim_l=0.0f, trim_r=0.0f;
    float   mic_gain=0.0f;
    float   touch_sens=0.007f;                          /* ★ 新增 */

    dooi_nvs_get_ota_url(ota_url, sizeof(ota_url));
    dooi_nvs_get_wifi_ssid(wifi_ssid, sizeof(wifi_ssid));
    dooi_nvs_get_eye_iris(&eye_iris);
    dooi_nvs_get_hw_invert(&mot_l, &mot_r, &srv_l, &srv_r);
    dooi_nvs_get_servo_trim(&trim_l, &trim_r);
    dooi_nvs_get_mic_gain(&mic_gain);
    touch_button_get_sensitivity(&touch_sens);   /* ★ 新增 */

    bool bt_enabled = (bool)app_ble_control_is_enabled();

    /* ★ 扩大缓冲并加入 touch_sens_x1000 字段 */
    char resp[620];
    int  len = snprintf(resp, sizeof(resp),
        "{\"bluetooth_enabled\":%s,\"ota_url\":\"%s\","
        "\"eye_iris\":%d,\"wifi_ssid\":\"%s\","
        "\"inv_mot_l\":%s,\"inv_mot_r\":%s,"
        "\"inv_srv_l\":%s,\"inv_srv_r\":%s,"
        "\"srv_trim_l\":%.1f,\"srv_trim_r\":%.1f,"
        "\"mic_gain\":%.1f,"
        "\"touch_sens_x1000\":%d}",             /* ★ 新增字段 */
        bt_enabled ? "true" : "false",
        ota_url, eye_iris, wifi_ssid,
        mot_l ? "true" : "false", mot_r ? "true" : "false",
        srv_l ? "true" : "false", srv_r ? "true" : "false",
        trim_l, trim_r,
        mic_gain,
        (int)(touch_sens * 1000.0f + 0.5f));    /* ★ 转为整数 */

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, resp, len);
}

/* action: set_bluetooth */
static esp_err_t action_set_bluetooth(httpd_req_t *req, const char *body)
{
    bool enabled = false;
    json_get_bool(body, "enabled", &enabled);
    if (enabled)
        app_ble_trigger_xbox_scan();
        // app_ble_control_start();
    // else
    //     app_ble_control_stop();
    ESP_LOGI(TAG, "BLE %s", enabled ? "ON" : "OFF");
    send_json_ok(req);
    return ESP_OK;
}

/* action: set_ota_url */
static esp_err_t action_set_ota_url(httpd_req_t *req, const char *body)
{
    char url[128] = {0};
    if (json_get_string(body, "url", url, sizeof(url))) {
        dooi_nvs_set_ota_url(url);
        ESP_LOGI(TAG, "OTA URL set: %s", url);
    }
    send_json_ok(req);
    return ESP_OK;
}

/* action: set_eye_iris */
static esp_err_t action_set_eye_iris(httpd_req_t *req, const char *body)
{
    int iris = 0;
    if (json_get_int(body, "iris", &iris)) {
        dooi_nvs_set_eye_iris((uint8_t)iris);
        ESP_LOGI(TAG, "Eye iris: %d", iris);
    }
    send_json_ok(req);
    return ESP_OK;
}

/* action: set_wifi — 先响应再重启（dooi_nvs_set_wifi 内部不返回） */
static esp_err_t action_set_wifi(httpd_req_t *req, const char *body)
{
    char ssid[32]     = {0};
    char password[64] = {0};

    if (!json_get_string(body, "ssid", ssid, sizeof(ssid))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
        return ESP_FAIL;
    }
    json_get_string(body, "password", password, sizeof(password));

    /* 先发送 HTTP 响应，确保客户端收到后再重启 */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "{\"status\":\"ok\"}", 15);

    dooi_nvs_set_wifi(ssid, password); /* 此函数内部重启，永不返回 */
    return ESP_OK;
}

static esp_err_t action_set_hw_invert(httpd_req_t *req, const char *body)
{
    bool mot_l=false, mot_r=false, srv_l=false, srv_r=false;
    
    json_get_bool(body, "inv_mot_l", &mot_l);
    json_get_bool(body, "inv_mot_r", &mot_r);
    json_get_bool(body, "inv_srv_l", &srv_l);
    json_get_bool(body, "inv_srv_r", &srv_r);
    
    // 保存到 NVS
    dooi_nvs_set_hw_invert(mot_l, mot_r, srv_l, srv_r);
    
    // 通知底层模块即时生效（需在底层实现这两个方法）
    app_motor_set_invert(mot_l, mot_r);
    app_servo_set_invert(srv_l, srv_r);
    
    ESP_LOGI(TAG, "Hardware invert updated");
    send_json_ok(req);
    return ESP_OK;
}
/* action: get_servo_trim — 单独读取 trim（可选，get 已包含） */
static esp_err_t action_get_servo_trim(httpd_req_t *req, const char *body)
{
    (void)body;
    float trim_l = 0.0f, trim_r = 0.0f;
    dooi_nvs_get_servo_trim(&trim_l, &trim_r);

    char resp[64];
    int len = snprintf(resp, sizeof(resp),
        "{\"srv_trim_l\":%.1f,\"srv_trim_r\":%.1f}", trim_l, trim_r);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, resp, len);
}

/* action: set_servo_trim — 保存 trim 并立即应用 */
static esp_err_t action_set_servo_trim(httpd_req_t *req, const char *body)
{
    /* JSON 中 trim 以 ×10 整数传输（0.1° 精度），避免浮点解析 */
    float trim_l = 0.0f, trim_r = 0.0f;
    int trim_l_x10 = 0, trim_r_x10 = 0;
    if (json_get_int(body, "srv_trim_l_x10", &trim_l_x10)) {
        trim_l = trim_l_x10 / 10.0f;
    }
    if (json_get_int(body, "srv_trim_r_x10", &trim_r_x10)) {
        trim_r = trim_r_x10 / 10.0f;
    }

    /* 保存到 NVS */
    dooi_nvs_set_servo_trim(trim_l, trim_r);

    /* 立即应用到舵机运行时 */
    app_servo_set_trim(SERVO_LEFT,  trim_l);
    app_servo_set_trim(SERVO_RIGHT, trim_r);

    ESP_LOGI(TAG, "Servo trim updated: L=%.1f R=%.1f", trim_l, trim_r);
    send_json_ok(req);
    return ESP_OK;
}

/* action: set_servo_angle — 舵机测试：下发逻辑角度（校准后可即时验证效果）
 * Body: {"srv_angle_l_x10":900,"srv_angle_r_x10":900,"duration_ms":300}
 * 角度单位为 0.1°（0 ~ 1800），镜像/反向/trim 均由 app_servo 内部处理。
 */
static esp_err_t action_set_servo_angle(httpd_req_t *req, const char *body)
{
    int l_x10 = 900, r_x10 = 900, dur = 300;

    json_get_int(body, "srv_angle_l_x10", &l_x10);
    json_get_int(body, "srv_angle_r_x10", &r_x10);
    json_get_int(body, "duration_ms",     &dur);

    /* 量程保护 */
    if (l_x10 < 0)    l_x10 = 0;      else if (l_x10 > 1800) l_x10 = 1800;
    if (r_x10 < 0)    r_x10 = 0;      else if (r_x10 > 1800) r_x10 = 1800;
    if (dur   < 0)    dur   = 0;      else if (dur   > 3000) dur   = 3000;

    float angle_l = l_x10 / 10.0f;
    float angle_r = r_x10 / 10.0f;

    app_servo_move_sync(angle_l, angle_r, (uint32_t)dur);

    ESP_LOGI(TAG, "Servo test: L=%.1f R=%.1f dur=%d", angle_l, angle_r, dur);
    send_json_ok(req);
    return ESP_OK;
}

/* action: set_mic_gain — 保存麦克风增益并立即应用 */
static esp_err_t action_set_mic_gain(httpd_req_t *req, const char *body)
{
    int gain_x10 = 0;
    float gain = 0.0f;
    if (json_get_int(body, "mic_gain_x10", &gain_x10)) {
        gain = gain_x10 / 10.0f;
    }

    dooi_nvs_set_mic_gain(gain);
    app_codec_gain_set(gain);   /* 立即应用，接口由外部声明 */

    ESP_LOGI(TAG, "Mic gain set: %.1f dB", gain);
    send_json_ok(req);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

/**
 * action: set_touch_sensitivity
 * Body: {"action":"set_touch_sensitivity","touch_sens_x1000":7}
 *       touch_sens_x1000 范围：3 ~ 50（对应 0.003 ~ 0.050）
 *                         值越小越灵敏
 */
static esp_err_t action_set_touch_sensitivity(httpd_req_t *req, const char *body)
{
    int sens_x1000 = 0;
    if (!json_get_int(body, "touch_sens_x1000", &sens_x1000)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing touch_sens_x1000");
        return ESP_FAIL;
    }

    /* 前端保护：后端再次 clamp（与 dooi_nvs 参数表量程一致：0.001 ~ 0.050） */
    if (sens_x1000 < 1)  sens_x1000 = 1;
    if (sens_x1000 > 50) sens_x1000 = 50;

    float ratio = sens_x1000 / 1000.0f;

    esp_err_t ret = touch_button_set_sensitivity(ratio);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set_touch_sensitivity failed: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recalibrate failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Touch sensitivity set: %.4f (x1000=%d)", ratio, sens_x1000);
    send_json_ok(req);
    return ESP_OK;
}

/* action: factory_reset — 清除 NVS 并重启 */
static esp_err_t action_factory_reset(httpd_req_t *req, const char *body)
{
    (void)body;

    /* 先把响应发出去，确保客户端收到再重启 */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "{\"status\":\"ok\"}", 15);

    ESP_LOGW(TAG, "Factory reset triggered, restarting...");

    vTaskDelay(pdMS_TO_TICKS(500));   /* 给 TCP 栈时间把响应刷出去 */
    dooi_nvs_factory_reset();         /* 清除 NVS，内部调用 esp_restart() */
    
    return ESP_OK;
}

/* ================================================================
 *  action 分发表（表驱动，扩展新 action 只需追加一行）
 * ================================================================ */
typedef esp_err_t (*action_fn_t)(httpd_req_t *req, const char *body);

typedef struct {
    const char  *name;
    action_fn_t  fn;
} action_entry_t;

static const action_entry_t s_actions[] = {
    { "get",           action_get            },
    { "set_bluetooth", action_set_bluetooth  },
    { "set_ota_url",   action_set_ota_url    },
    { "set_eye_iris",  action_set_eye_iris   },
    { "set_wifi",      action_set_wifi       },
    { "set_hw_invert", action_set_hw_invert  }, 
    { "get_servo_trim",   action_get_servo_trim   }, 
    { "set_servo_trim",   action_set_servo_trim   }, 
    { "set_servo_angle",  action_set_servo_angle  }, /* 舵机测试 */
    { "set_mic_gain",  action_set_mic_gain  },
    { "factory_reset",    action_factory_reset  }, 
    { "set_touch_sensitivity",  action_set_touch_sensitivity  }, /* ★ 新增 */
    /* 在此追加新 action，无需修改其他任何代码 */
};
#define ACTION_COUNT (sizeof(s_actions) / sizeof(s_actions[0]))

/* ================================================================
 *  Handler: POST /api/config
 * ================================================================ */
static esp_err_t api_config_handler(httpd_req_t *req)
{
    char body[512];
    int  received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[received] = '\0';
    ESP_LOGD(TAG, "POST /api/config body: %s", body);

    char action[32] = {0};
    if (!json_get_string(body, "action", action, sizeof(action))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing action");
        return ESP_FAIL;
    }

    for (size_t i = 0; i < ACTION_COUNT; i++) {
        if (strcmp(action, s_actions[i].name) == 0) {
            return s_actions[i].fn(req, body);
        }
    }

    ESP_LOGW(TAG, "Unknown action: %s", action);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown action");
    return ESP_FAIL;
}

/* ================================================================
 *  路由表
 *  新增页面在此追加一行即可
 * ================================================================ */
static const web_route_t s_routes[] = {
    { "/settings",   HTTP_GET,  settings_get_handler, false },
    { "/api/config", HTTP_POST, api_config_handler,   false },
};

/* ================================================================
 *  模块初始化入口（由 app_webserver.c 调用，httpd 启动前）
 * ================================================================ */
void app_web_settings_init(void)
{
    app_web_register_routes(WEB_SERVER_CTRL,
                            s_routes,
                            sizeof(s_routes) / sizeof(s_routes[0]));
    ESP_LOGI(TAG, "%d routes registered", (int)(sizeof(s_routes) / sizeof(s_routes[0])));
}