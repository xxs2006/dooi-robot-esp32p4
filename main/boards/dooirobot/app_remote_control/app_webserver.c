/**
 * @file app_webserver.c
 * @brief Web 服务器核心框架（单通配符路由版本）
 *
 * 职责:
 *   - 启动 / 停止两个 httpd 实例 (Port 80 + Port 8081)
 *   - 维护软件路由表，驱动子模块填表 (app_web_register_routes)
 *   - 全局仅调用 4 次 httpd_register_uri_handler，后续增加页面无需新增
 *   - 管理 WebSocket 客户端 fd 列表
 *   - mDNS 初始化
 *
 * 不包含任何具体页面/API 逻辑，这些由子模块负责。
 */

#include "app_webserver.h"

#include <string.h>
#include <sys/socket.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "app_ui_logic.h"

/* ---- 子模块初始化声明（各自 .c 文件实现） ---- */
void app_web_index_init(void);
void app_web_settings_init(void);
void app_web_stream_init(void);
void app_web_behavior_init(void);
void app_web_eyes_init(void);   
void app_web_studio_init(void); 

#define TAG           "APP_WEBSVR"
#define MDNS_HOSTNAME "dooirobot"

/* ================================================================
 *  服务器句柄
 * ================================================================ */
static httpd_handle_t s_ctrl_server   = NULL;
static httpd_handle_t s_stream_server = NULL;

httpd_handle_t app_webserver_get_ctrl_handle(void)   { return s_ctrl_server;   }
httpd_handle_t app_webserver_get_stream_handle(void) { return s_stream_server; }

/* ================================================================
 *  软件路由表
 *  子模块调用 app_web_register_routes() 填充，
 *  httpd 启动后由通配符 handler 做软件分发。
 * ================================================================ */
static web_route_t s_ctrl_routes[WEB_MAX_ROUTES];
static size_t      s_ctrl_route_count = 0;

static web_route_t s_stream_routes[WEB_MAX_ROUTES];
static size_t      s_stream_route_count = 0;

void app_web_register_routes(web_server_t target,
                              const web_route_t *routes,
                              size_t count)
{
    web_route_t *table;
    size_t      *cnt;

    if (target == WEB_SERVER_CTRL) {
        table = s_ctrl_routes;
        cnt   = &s_ctrl_route_count;
    } else {
        table = s_stream_routes;
        cnt   = &s_stream_route_count;
    }

    for (size_t i = 0; i < count; i++) {
        if (*cnt >= WEB_MAX_ROUTES) {
            ESP_LOGE(TAG, "Route table full! Increase WEB_MAX_ROUTES");
            break;
        }
        table[(*cnt)++] = routes[i];
    }
}

/* ================================================================
 *  路径匹配辅助：忽略查询字符串，只比较路径部分
 *  例：pattern="/capture"  uri="/capture?t=1234567" → true
 * ================================================================ */
static bool uri_path_match(const char *pattern, const char *uri)
{
    size_t plen = strlen(pattern);
    return strncmp(pattern, uri, plen) == 0 &&
           (uri[plen] == '\0' || uri[plen] == '?' || uri[plen] == '#');
}

/* ================================================================
 *  通配符 dispatcher（核心路由分发逻辑）
 *  按 method + URI路径 匹配（忽略查询字符串），未命中返回 404。
 * ================================================================ */
static esp_err_t ctrl_dispatch(httpd_req_t *req)
{
    for (size_t i = 0; i < s_ctrl_route_count; i++) {
        if (s_ctrl_routes[i].is_websocket) continue;  /* WS 已独立注册，跳过 */
        if (s_ctrl_routes[i].method == req->method &&
            uri_path_match(s_ctrl_routes[i].uri, req->uri)) {
            return s_ctrl_routes[i].handler(req);
        }
    }
    ESP_LOGW(TAG, "404: %d %s", req->method, req->uri);
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

static esp_err_t stream_dispatch(httpd_req_t *req)
{
    for (size_t i = 0; i < s_stream_route_count; i++) {
        if (s_stream_routes[i].method == req->method &&
            strcmp(s_stream_routes[i].uri, req->uri) == 0) {
            return s_stream_routes[i].handler(req);
        }
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

/* ================================================================
 *  WebSocket 客户端管理
 * ================================================================ */
static int s_ws_fds[WEB_WS_MAX_CLI];

static void ws_clients_init(void)
{
    for (int i = 0; i < WEB_WS_MAX_CLI; i++) s_ws_fds[i] = -1;
}

void app_ws_client_add(int fd)
{
    for (int i = 0; i < WEB_WS_MAX_CLI; i++) {
        if (s_ws_fds[i] == -1) {
            s_ws_fds[i] = fd;
            return;
        }
    }
    ESP_LOGW(TAG, "WS slots full, fd=%d rejected", fd);
}

void app_ws_client_remove(int fd)
{
    for (int i = 0; i < WEB_WS_MAX_CLI; i++) {
        if (s_ws_fds[i] == fd) {
            s_ws_fds[i] = -1;
            return;
        }
    }
}

bool app_webserver_has_ws_client(void)
{
    for (int i = 0; i < WEB_WS_MAX_CLI; i++) {
        if (s_ws_fds[i] != -1) return true;
    }
    return false;
}

/* ================================================================
 *  Socket 关闭回调 — 清理 WS 客户端记录
 * ================================================================ */
static void on_close(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    app_ws_client_remove(sockfd);

    app_event_t evt = {.type = APP_EVT_WEB_SERVER_DISCONNECT};
    app_ui_logic_post(&evt, false);

    ESP_LOGD(TAG, "Socket closed fd=%d", sockfd);
    close(sockfd);
}

/* ================================================================
 *  mDNS 初始化
 * ================================================================ */
static void mdns_setup(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed");
        return;
    }
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set(MDNS_HOSTNAME);
    mdns_service_add("web",    "_http", "_tcp", WEB_CTRL_PORT,   NULL, 0);
    mdns_service_add("stream", "_http", "_tcp", WEB_STREAM_PORT, NULL, 0);
    mdns_txt_item_t txt[] = { {"path", "/"} };
    mdns_service_txt_set("_http", "_tcp", txt, 1);
    ESP_LOGI(TAG, "mDNS: %s.local ports %d/%d",
             MDNS_HOSTNAME, WEB_CTRL_PORT, WEB_STREAM_PORT);
}

/* ================================================================
 *  控制服务器启动 (Port 80)
 *
 *  httpd_register_uri_handler 调用次数: 3
 *    #1 — /ws        (WebSocket 必须独立注册，is_websocket=true)
 *    #2 — / GET     (所有 GET 请求通配分发)
 *    #3 — / POST    (所有 POST 请求通配分发)
 * ================================================================ */
static esp_err_t ctrl_server_start(void)
{
    /* Step 1: 子模块填充路由表（此时 httpd 尚未启动） */
    app_web_index_init();
    app_web_settings_init();
    app_web_behavior_init();
    app_web_eyes_init();
    app_web_studio_init();

    /* Step 2: 启动 httpd
     * /studio 页面已被设备化打包为"单 JS + css 内联"，浏览器对页面最多
     * 同时发 ~2 个请求（html + bundle，wasm 页面就绪后才拉），默认 7 路够用。
     * 注意：max_open_sockets 上限 = LWIP_MAX_SOCKETS - 3；如确需更大并发，
     * 需同时调大 sdkconfig CONFIG_LWIP_MAX_SOCKETS，否则 httpd_start 失败。 */
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = WEB_CTRL_PORT;
    cfg.ctrl_port        = 32768;
    cfg.max_open_sockets = 7;
    cfg.close_fn         = on_close;
    cfg.stack_size       = 12288;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;

    esp_err_t ret = httpd_start(&s_ctrl_server, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ctrl server start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Step 3: 从路由表中找出 WS handler，独立注册（框架要求） */
    httpd_uri_t uri_ws = {
        .uri          = "/ws",
        .method       = HTTP_GET,
        .handler      = NULL,
        .is_websocket = true,
    };
    for (size_t i = 0; i < s_ctrl_route_count; i++) {
        if (s_ctrl_routes[i].is_websocket &&
            strcmp(s_ctrl_routes[i].uri, "/ws") == 0) {
            uri_ws.handler = s_ctrl_routes[i].handler;
            break;
        }
    }
    if (uri_ws.handler) {
        httpd_register_uri_handler(s_ctrl_server, &uri_ws);       /* 注册 #1 */
    }

    /* Step 4: 注册 GET / POST 通配符，覆盖所有其余路由 */
    static const httpd_uri_t uri_get_wc = {
        .uri     = "/*",
        .method  = HTTP_GET,
        .handler = ctrl_dispatch,
    };
    static const httpd_uri_t uri_post_wc = {
        .uri     = "/*",
        .method  = HTTP_POST,
        .handler = ctrl_dispatch,
    };
    httpd_register_uri_handler(s_ctrl_server, &uri_get_wc);       /* 注册 #2 */
    httpd_register_uri_handler(s_ctrl_server, &uri_post_wc);      /* 注册 #3 */

    ESP_LOGI(TAG, "Ctrl server on port %d, %d routes, 3 handlers registered",
             WEB_CTRL_PORT, (int)s_ctrl_route_count);
    return ESP_OK;
}

/* ================================================================
 *  视频流服务器启动 (Port 8081)
 *
 *  ★ 关键：流服务器【不使用通配符】，直接精确注册每条路由。
 *
 *  原因：MJPEG 流是阻塞式长连接，ESP-IDF httpd 在通配符模式下
 *  对长连接处理有问题；精确注册与改架构前行为一致，可靠稳定。
 *  只有 Port 80 的控制服务器使用通配符架构。
 * ================================================================ */
static esp_err_t stream_server_start(void)
{
    /* Step 1: 子模块填路由表 */
    app_web_stream_init();   /* 注册到 WEB_SERVER_STREAM */

    /* Step 2: 启动 httpd —— 注意：不设置 uri_match_fn（用默认精确匹配） */
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = WEB_STREAM_PORT;
    cfg.ctrl_port        = 32769;
    cfg.max_open_sockets = 2;
    cfg.lru_purge_enable = false; 
    cfg.stack_size       = 4096;      

    esp_err_t ret = httpd_start(&s_stream_server, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Stream server start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Step 3: 直接精确注册流服务器的每条路由（不用通配符分发） */
    for (size_t i = 0; i < s_stream_route_count; i++) {
        httpd_uri_t uri = {
            .uri          = s_stream_routes[i].uri,
            .method       = s_stream_routes[i].method,
            .handler      = s_stream_routes[i].handler,
            .is_websocket = s_stream_routes[i].is_websocket,
        };
        httpd_register_uri_handler(s_stream_server, &uri);
        ESP_LOGI(TAG, "Stream route: %s", uri.uri);
    }

    ESP_LOGI(TAG, "Stream server on port %d, %d routes (exact match)",
             WEB_STREAM_PORT, (int)s_stream_route_count);
    return ESP_OK;
}

/* ================================================================
 *  启动任务（异步，避免阻塞调用方）
 * ================================================================ */
static void webserver_start_task(void *arg)
{
    ws_clients_init();
#if USE_WEB_MDNS
    mdns_setup();
#endif
    ctrl_server_start();
    stream_server_start();
    vTaskDelete(NULL);
}

/* ================================================================
 *  对外接口
 * ================================================================ */
esp_err_t app_webserver_start(void)
{
    BaseType_t ok = xTaskCreate(webserver_start_task,
                                "websvr_init",
                                4096, NULL, 5, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

void app_webserver_stop(void)
{
    if (s_ctrl_server) {
        httpd_stop(s_ctrl_server);
        s_ctrl_server = NULL;
    }
    if (s_stream_server) {
        httpd_stop(s_stream_server);
        s_stream_server = NULL;
    }
#if USE_WEB_MDNS
    mdns_free();
#endif
    ESP_LOGI(TAG, "Web servers stopped");
}