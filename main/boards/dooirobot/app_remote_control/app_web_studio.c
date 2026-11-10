/**
 * @file app_web_studio.c
 * @brief Little Eyes Studio 网页资源伺服（设备自托管）
 *
 * 资源由主 CMake EMBED_FILES 嵌入（目录 component/little_eyes/eyes-studio/web 之下，
 * 由 little_eyes/tools/sync_to_board.py 镜像生成），符号按"文件名"生成
 * （IDF 规则：目录不影响符号名，basename 用 _ 替换分隔符）。
 *
 * 路由（Port 80）：
 *   /studio                         -> 302 /studio/（页面内容 = 嵌入的 studio.html）
 *   /studio/、/studio/studio.html   -> studio.html（相对引用 base=/studio/）
 *   /studio/css/eyes.css
 *   /studio/js/{names,preset_data,schema,engine,export,bridge,app}.js
 *   /studio/lib/eyes_studio.js
 *   /studio/lib/eyes_studio.wasm    (application/wasm，wasm 加载必需)
 *
 * 页面内的 API 调用走同源 /api/eyes 接口（见 app_web_eyes.c）。
 */

#include "app_webserver.h"

#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"

#define TAG "WEB_STUDIO"

/* ---- 嵌入资源（CMake EMBED_FILES -> basename 符号） ---- */
extern const uint8_t studio_html_start[]      asm("_binary_studio_html_start");
extern const uint8_t studio_html_end[]        asm("_binary_studio_html_end");
extern const uint8_t eyes_css_start[]         asm("_binary_eyes_css_start");
extern const uint8_t eyes_css_end[]           asm("_binary_eyes_css_end");
extern const uint8_t names_js_start[]         asm("_binary_names_js_start");
extern const uint8_t names_js_end[]           asm("_binary_names_js_end");
extern const uint8_t preset_data_js_start[]   asm("_binary_preset_data_js_start");
extern const uint8_t preset_data_js_end[]     asm("_binary_preset_data_js_end");
extern const uint8_t schema_js_start[]        asm("_binary_schema_js_start");
extern const uint8_t schema_js_end[]          asm("_binary_schema_js_end");
extern const uint8_t engine_js_start[]        asm("_binary_engine_js_start");
extern const uint8_t engine_js_end[]          asm("_binary_engine_js_end");
extern const uint8_t export_js_start[]        asm("_binary_export_js_start");
extern const uint8_t export_js_end[]          asm("_binary_export_js_end");
extern const uint8_t bridge_js_start[]        asm("_binary_bridge_js_start");
extern const uint8_t bridge_js_end[]          asm("_binary_bridge_js_end");
extern const uint8_t app_js_start[]           asm("_binary_app_js_start");
extern const uint8_t app_js_end[]             asm("_binary_app_js_end");
extern const uint8_t eyes_studio_js_start[]   asm("_binary_eyes_studio_js_start");
extern const uint8_t eyes_studio_js_end[]     asm("_binary_eyes_studio_js_end");
extern const uint8_t eyes_studio_wasm_start[] asm("_binary_eyes_studio_wasm_start");
extern const uint8_t eyes_studio_wasm_end[]   asm("_binary_eyes_studio_wasm_end");

typedef struct {
    const char   *uri;
    const char   *ctype;
    const uint8_t *start;
    const uint8_t *end;
} studio_asset_t;

static const studio_asset_t s_assets[] = {
    { "/studio/",               "text/html; charset=utf-8",       studio_html_start,      studio_html_end },
    { "/studio/studio.html",    "text/html; charset=utf-8",       studio_html_start,      studio_html_end },
    { "/studio/css/eyes.css",   "text/css; charset=utf-8",        eyes_css_start,         eyes_css_end },
    { "/studio/js/names.js",        "application/javascript; charset=utf-8", names_js_start,       names_js_end },
    { "/studio/js/preset_data.js",  "application/javascript; charset=utf-8", preset_data_js_start, preset_data_js_end },
    { "/studio/js/schema.js",       "application/javascript; charset=utf-8", schema_js_start,      schema_js_end },
    { "/studio/js/engine.js",       "application/javascript; charset=utf-8", engine_js_start,      engine_js_end },
    { "/studio/js/export.js",       "application/javascript; charset=utf-8", export_js_start,      export_js_end },
    { "/studio/js/bridge.js",       "application/javascript; charset=utf-8", bridge_js_start,      bridge_js_end },
    { "/studio/js/app.js",          "application/javascript; charset=utf-8", app_js_start,         app_js_end },
    { "/studio/lib/eyes_studio.js", "application/javascript; charset=utf-8", eyes_studio_js_start, eyes_studio_js_end },
    { "/studio/lib/eyes_studio.wasm","application/wasm",                      eyes_studio_wasm_start, eyes_studio_wasm_end },
};

static const studio_asset_t *asset_by_uri(const char *uri)
{
    for (size_t i = 0; i < sizeof(s_assets) / sizeof(s_assets[0]); i++) {
        /* uri_path_match 语义：忽略查询字符串 */
        size_t plen = strlen(s_assets[i].uri);
        if (strncmp(s_assets[i].uri, uri, plen) == 0 &&
            (uri[plen] == '\0' || uri[plen] == '?' || uri[plen] == '#')) {
            return &s_assets[i];
        }
    }
    return NULL;
}

/* ================================================================
 *  Handler: GET /studio -> 302 /studio/
 *  页面里的 css/js 是相对引用（css/eyes.css 等），必须带尾斜杠保证
 *  base 为 /studio/，否则相对路径会解析到根目录。
 * ================================================================ */
static esp_err_t studio_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/studio/");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_sendstr(req, "");
}

/* ================================================================
 *  Handler: GET /studio/...（通用静态资源）
 * ================================================================ */
static esp_err_t studio_asset_handler(httpd_req_t *req)
{
    const studio_asset_t *a = asset_by_uri(req->uri);
    if (!a) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, a->ctype);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, (const char *)a->start,
                           (ssize_t)(a->end - a->start));
}

/* ================================================================
 *  路由表
 * ================================================================ */
static const web_route_t s_routes[] = {
    { "/studio",                     HTTP_GET, studio_redirect_handler, false },
    { "/studio/",                    HTTP_GET, studio_asset_handler,    false },
    { "/studio/studio.html",         HTTP_GET, studio_asset_handler,    false },
    { "/studio/css/eyes.css",        HTTP_GET, studio_asset_handler, false },
    { "/studio/js/names.js",         HTTP_GET, studio_asset_handler, false },
    { "/studio/js/preset_data.js",   HTTP_GET, studio_asset_handler, false },
    { "/studio/js/schema.js",        HTTP_GET, studio_asset_handler, false },
    { "/studio/js/engine.js",        HTTP_GET, studio_asset_handler, false },
    { "/studio/js/export.js",        HTTP_GET, studio_asset_handler, false },
    { "/studio/js/bridge.js",        HTTP_GET, studio_asset_handler, false },
    { "/studio/js/app.js",           HTTP_GET, studio_asset_handler, false },
    { "/studio/lib/eyes_studio.js",  HTTP_GET, studio_asset_handler, false },
    { "/studio/lib/eyes_studio.wasm",HTTP_GET, studio_asset_handler, false },
};

void app_web_studio_init(void)
{
    app_web_register_routes(WEB_SERVER_CTRL, s_routes,
                            sizeof(s_routes) / sizeof(s_routes[0]));
    ESP_LOGI(TAG, "%d studio asset routes registered",
             (int)(sizeof(s_routes) / sizeof(s_routes[0])));
}
