/**
 * @file app_web_index.c
 * @brief 首页 / JPEG 快照 / WebSocket 控制
 *
 * 负责路由:
 *   GET  /         — 返回 index.html
 *   GET  /capture  — 单帧 JPEG 快照
 *   GET  /ws       — WebSocket 控制通道 (is_websocket=true)
 *
 * 初始化:
 *   app_web_index_init() 由 app_webserver.c 在 httpd 启动前调用，
 *   仅向路由表填充条目，不直接调用 httpd_register_uri_handler。
 */

#include "app_webserver.h"
#include "remote_protocol.h"
#include "dooi_camera_iface.h"
#include "app_ui_logic.h"

#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"

#define TAG "WEB_INDEX"

/* ---- 嵌入 HTML ---- */
extern const uint8_t index_html_start[]   asm("_binary_index_html_start");
extern const uint8_t index_html_end[]     asm("_binary_index_html_end");

/* ================================================================
 *  Handler: GET /
 * ================================================================ */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req,
        (const char *)index_html_start,
        (ssize_t)(index_html_end - index_html_start));
}

/* ================================================================
 *  Handler: GET /capture — 单帧 JPEG 快照
 * ================================================================ */
static esp_err_t capture_get_handler(httpd_req_t *req)
{
    FrameBuffer frame = camera_get_web_frame();
    if (frame.len == 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, (const char *)frame.data, frame.len);
}

/* ================================================================
 *  Handler: GET /ws — WebSocket 控制指令
 * ================================================================ */
static esp_err_t ws_handler(httpd_req_t *req)
{
    /* 握手阶段 */
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        app_ws_client_add(fd);

        app_event_t evt = {.type = APP_EVT_WEB_SERVER_CONNECT};
        app_ui_logic_post(&evt, false);
        ESP_LOGI(TAG, "WS client connected fd=%d", fd);
        return ESP_OK;
    }

    /* 读取帧长度 */
    httpd_ws_frame_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = HTTPD_WS_TYPE_BINARY;

    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK) return ret;
    if (pkt.len == 0) return ESP_OK;
    if (pkt.len > 64) {
        ESP_LOGW(TAG, "WS frame too large: %d", (int)pkt.len);
        return ESP_ERR_INVALID_SIZE;
    }

    /* 读取 payload */
    uint8_t buf[64];
    pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret != ESP_OK) return ret;

    /* 协议处理 */
    remote_response_t rsp;
    remote_protocol_handle(buf, pkt.len, &rsp);

    /* 回传应答 */
    httpd_ws_frame_t rsp_frame = {
        .type    = HTTPD_WS_TYPE_BINARY,
        .payload = (uint8_t *)&rsp.pkt,
        .len     = rsp.len,
        .final   = true,
    };
    return httpd_ws_send_frame(req, &rsp_frame);
}

/* ================================================================
 *  路由表
 *  新增页面在此追加一行即可，无需修改 app_webserver.c
 * ================================================================ */
static const web_route_t s_routes[] = {
    { "/",        HTTP_GET, root_get_handler,    false },
    { "/capture", HTTP_GET, capture_get_handler, false },
    { "/ws",      HTTP_GET, ws_handler,          true  },  /* is_websocket=true */
};

/* ================================================================
 *  模块初始化入口（由 app_webserver.c 调用，httpd 启动前）
 * ================================================================ */
void app_web_index_init(void)
{
    app_web_register_routes(WEB_SERVER_CTRL,
                            s_routes,
                            sizeof(s_routes) / sizeof(s_routes[0]));
    ESP_LOGI(TAG, "%d routes registered", (int)(sizeof(s_routes) / sizeof(s_routes[0])));
}