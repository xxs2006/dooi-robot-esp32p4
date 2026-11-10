/**
 * @file app_web_stream.c
 * @brief MJPEG 视频流 (Port 8081)
 *
 * 负责路由:
 *   GET /stream — 持续推送 MJPEG 帧，直到客户端断开
 *
 * 初始化:
 *   app_web_stream_init() 由 app_webserver.c 在流服务器启动前调用。
 */

#include "app_webserver.h"
#include "dooi_camera_iface.h"

#include <sys/socket.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "WEB_STREAM"

#define MJPEG_BOUNDARY "frame" 

static const char MJPEG_CONTENT_TYPE[] = "multipart/x-mixed-replace; boundary=" MJPEG_BOUNDARY; 
static const char MJPEG_PART_HEADER_FMT[] = "--" MJPEG_BOUNDARY "\r\n" "Content-Type: image/jpeg\r\n" "Content-Length: %zu\r\n\r\n";

/* ================================================================
 *  检查 socket 是否仍然活跃
 *  使用 MSG_PEEK | MSG_DONTWAIT 非阻塞探测，不消耗数据
 * ================================================================ */
static bool is_socket_alive(int sockfd)
{
    // 方法 1：尝试 peek 1 字节
    char buf[1];
    int ret = recv(sockfd, buf, 1, MSG_PEEK | MSG_DONTWAIT);
    
    if (ret == 0) {
        // 对端已关闭连接
        return false;
    }
    
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 没有数据可读，但连接正常
            return true;
        }
        // 其他错误（如 ECONNRESET）
        ESP_LOGD(TAG, "Socket error: %d (%s)", errno, strerror(errno));
        return false;
    }
    
    // ret > 0：有数据可读（不应该发生在 MJPEG 流中）
    return true;
}

/* ================================================================
 *  Handler: GET /stream
 * ================================================================ */
static esp_err_t stream_handler(httpd_req_t *req)
{
    int sock = httpd_req_to_sockfd(req);

    httpd_resp_set_type(req, MJPEG_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "X-Framerate", "15");

    char part_hdr[128];
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "MJPEG start fd=%d uri=%s", sock, req->uri);

    while (ret == ESP_OK) {
        FrameBuffer frame = camera_get_web_frame();

        if (frame.len == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!is_socket_alive(sock)) {
            ESP_LOGW(TAG, "MJPEG dead peer fd=%d", sock);
            break;
        }

        int hdr_len = snprintf(part_hdr, sizeof(part_hdr),
                               MJPEG_PART_HEADER_FMT, frame.len);

        ret = httpd_resp_send_chunk(req, part_hdr, hdr_len);
        if (ret != ESP_OK) break;

        ret = httpd_resp_send_chunk(req, (const char *)frame.data, frame.len);
        if (ret != ESP_OK) break;

        ret = httpd_resp_send_chunk(req, "\r\n", 2);
        if (ret != ESP_OK) break;

        vTaskDelay(pdMS_TO_TICKS(33));
    }

    ESP_LOGI(TAG, "MJPEG end fd=%d ret=%d", sock, ret);
    return ESP_OK;
}

/* ================================================================
 *  路由表
 * ================================================================ */
static const web_route_t s_routes[] = {
    { "/stream", HTTP_GET, stream_handler, false },
};

/* ================================================================
 *  模块初始化入口（由 app_webserver.c 调用，httpd 启动前）
 * ================================================================ */
void app_web_stream_init(void)
{
    app_web_register_routes(WEB_SERVER_STREAM,
                            s_routes,
                            sizeof(s_routes) / sizeof(s_routes[0]));
    ESP_LOGI(TAG, "%d routes registered", (int)(sizeof(s_routes) / sizeof(s_routes[0])));
}