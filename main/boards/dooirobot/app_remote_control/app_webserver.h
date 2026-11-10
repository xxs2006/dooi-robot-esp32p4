/**
 * @file app_webserver.h
 * @brief HTTP + WebSocket + MJPEG 视频流 Web 服务器
 *
 * 架构说明:
 *   app_webserver.c    — 核心框架: 服务器生命周期、单通配符路由分发、WS 客户端管理、mDNS
 *   app_web_index.c    — 首页 / capture 快照 / WebSocket 控制 (/  /gamepad  /capture  /ws)
 *   app_web_settings.c — 设置页 / REST 配置 API            (/settings  /api/config)
 *   app_web_stream.c   — MJPEG 视频流 (Port 8081)          (/stream)
 *
 * URI 注册机制 (全局仅调用 4 次 httpd_register_uri_handler):
 *   Port 80  : /ws 独立注册(1次) + GET 通配 /(1次) + POST 通配 /(1次)
 *   Port 8081: GET 通配 /(1次)
 *   各子模块调用 app_web_register_routes() 填充路由表，
 *   由核心统一做软件路由分发，后续增加页面无需新增注册调用。
 */

#ifndef APP_WEBSERVER_H
#define APP_WEBSERVER_H

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USE_WEB_MDNS      0

/* ================================================================
 *  公共配置常量
 * ================================================================ */
#define WEB_CTRL_PORT    80
#define WEB_STREAM_PORT  8081
#define WEB_WS_MAX_CLI   1
#define WEB_MAX_ROUTES   64   /* index+settings+stream+behavior+eyes+studio 静态资源 */

/* ================================================================
 *  目标服务器选择
 * ================================================================ */
typedef enum {
    WEB_SERVER_CTRL   = 0,  /**< 控制服务器 Port 80   */
    WEB_SERVER_STREAM = 1,  /**< 视频流服务器 Port 8081 */
} web_server_t;

/* ================================================================
 *  路由条目
 *  子模块使用此结构体填写路由，而非直接操作 httpd_uri_t
 * ================================================================ */
typedef struct {
    const char      *uri;           /**< 路径字符串，如 "/capture"     */
    httpd_method_t   method;        /**< HTTP_GET / HTTP_POST 等        */
    esp_err_t       (*handler)(httpd_req_t *req);
    bool             is_websocket;  /**< 仅 WebSocket 端点需置 true     */
} web_route_t;

/* ================================================================
 *  子模块路由注册接口
 *  在 httpd 启动之前调用（由 app_webserver.c 在 init 时驱动）
 *
 * @param target  WEB_SERVER_CTRL 或 WEB_SERVER_STREAM
 * @param routes  路由数组
 * @param count   数组元素个数
 * ================================================================ */
void app_web_register_routes(web_server_t target,
                              const web_route_t *routes,
                              size_t count);

/* ================================================================
 *  WebSocket 客户端管理（供子模块使用）
 * ================================================================ */
void app_ws_client_add(int fd);
void app_ws_client_remove(int fd);
bool app_webserver_has_ws_client(void);

/* ================================================================
 *  服务器句柄访问（子模块一般不需要，仅高级用途）
 * ================================================================ */
httpd_handle_t app_webserver_get_ctrl_handle(void);
httpd_handle_t app_webserver_get_stream_handle(void);

/* ================================================================
 *  对外接口
 * ================================================================ */

/**
 * @brief 异步启动 Web 服务器（内部创建初始化任务）
 *        Port 80  : HTML 页面 / WebSocket / JPEG 快照
 *        Port 8081: MJPEG 视频流
 * @return ESP_OK 任务创建成功
 */
esp_err_t app_webserver_start(void);

/**
 * @brief 停止 Web 服务器并释放资源（包括 mDNS）
 */
void app_webserver_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_WEBSERVER_H */