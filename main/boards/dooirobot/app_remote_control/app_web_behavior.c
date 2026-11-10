/**
 * @file app_web_behavior.c
 * @brief 行为资源 JSON 编辑页 + REST API
 *
 * 路由（Port 80，经 ctrl_dispatch 软件分发）:
 *   GET  /behavior              — 返回 behavior.html
 *   GET  /api/behavior/list      — 列目录 (?type=scenes|status|asset&channel=eye|led|motor|servo|audio)
 *   GET  /api/behavior/file      — 取文件原文 (?path=<rel>)
 *   POST /api/behavior/file      — 保存/删除文件
 *
 * POST body（JSON）:
 *   {"action":"save",   "path":"scenes/x.json", "content": { ...json对象... }}
 *   {"action":"delete", "path":"scenes/x.json"}
 *
 * 目标存储为 SPIFFS "behavior" 分区（挂载 /resources），与 app_behavior_fs 共用。
 * httpd 默认单线程串行分发，故共享静态 PSRAM 缓冲安全（同 app_web_custom_seq 约定）。
 */

#include "app_webserver.h"
#include "app_behavior_fs.h"
#include "app_behavior.h"
#include "app_ui_logic.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cJSON.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"

#define TAG "WEB_BEHAVIOR"

/* ---- 嵌入 HTML ---- */
extern const uint8_t behavior_html_start[] asm("_binary_behavior_html_start");
extern const uint8_t behavior_html_end[]   asm("_binary_behavior_html_end");

/* ================================================================
 *  常量
 * ================================================================ */
#define BEH_RESP_BUF_SIZE   (8 * 1024)    /* 列表响应缓冲 */
#define BEH_BODY_BUF_SIZE   (40 * 1024)   /* POST body（单文件上限 32KB，留余量） */

/* ----------------------------------------------------------------
 *  PSRAM 静态缓冲（一次性分配，避免反复申请）
 * ---------------------------------------------------------------- */
static char *s_resp_buf  = NULL;
static char *s_body_buf  = NULL;
static bool  s_inited    = false;

static void bufs_init(void)
{
    if (s_inited) return;
    s_resp_buf = (char *)heap_caps_calloc(1, BEH_RESP_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_body_buf = (char *)heap_caps_calloc(1, BEH_BODY_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_resp_buf || !s_body_buf) {
        ESP_LOGE(TAG, "Failed to alloc PSRAM buffers");
        return;
    }
    s_inited = true;
    ESP_LOGI(TAG, "PSRAM buffers ready (resp=%d, body=%d)",
             BEH_RESP_BUF_SIZE, BEH_BODY_BUF_SIZE);
}

/* ================================================================
 *  HTTP 工具
 * ================================================================ */
static void send_json_ok(httpd_req_t *req, const char *extra)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    if (extra) {
        char buf[256];
        int len = snprintf(buf, sizeof(buf), "{\"status\":\"ok\",%s}", extra);
        httpd_resp_send(req, buf, len);
    } else {
        httpd_resp_send(req, "{\"status\":\"ok\"}", 15);
    }
}

static void send_json_err(httpd_req_t *req, const char *msg)
{
    char buf[160];
    int len = snprintf(buf, sizeof(buf), "{\"status\":\"error\",\"error\":\"%s\"}", msg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, buf, len);
}

/* 安全追加：clamp 返回值到剩余缓冲区 */
static int safe_append(char *buf, int buf_size, int *pos, const char *fmt, ...)
{
    int remain = buf_size - *pos;
    if (remain <= 1) return 0;
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buf + *pos, remain, fmt, args);
    va_end(args);
    if (written < 0) return 0;
    if (written >= remain) written = remain - 1;
    *pos += written;
    return written;
}

/* 边界安全的字符串拷贝：始终以 NUL 结尾（不使用 strncpy，避免 -Wstringop-truncation） */
static void safe_strcpy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    size_t i = 0;
    const char *s = src ? src : "";
    while (s[i] != '\0' && i + 1 < dst_size) {
        dst[i] = s[i];
        i++;
    }
    dst[i] = '\0';
}

/* 在已有路径后追加一段（必要时加 '/' 分隔），边界安全且始终 NUL 结尾 */
static void path_append(char *path, size_t path_size, const char *seg)
{
    if (!path || path_size == 0 || !seg) return;
    size_t len = strlen(path);
    if (len > 0 && path[len - 1] != '/' && len + 1 < path_size) {
        path[len] = '/';
        len++;
        path[len] = '\0';
    }
    while (*seg != '\0' && len + 1 < path_size) {
        path[len] = *seg;
        len++;
        seg++;
    }
    path[len] = '\0';
}

/* 原地 URL 解码（%XX → 字节，+ → 空格）。返回解码后长度。
 * ESP-IDF 的 httpd_query_key_value 不做 URL 解码，前端 encodeURIComponent
 * 会把 '/' 编码成 %2F，导致 path_is_safe 的 strncmp 失败 → 全部 bad path。
 */
static size_t url_decode_inplace(char *s)
{
    if (!s) return 0;
    size_t rd = 0, wr = 0;
    while (s[rd]) {
        if (s[rd] == '+') { s[wr++] = ' '; rd++; }
        else if (s[rd] == '%' && s[rd+1] && s[rd+2]) {
            char hex[3] = { s[rd+1], s[rd+2], 0 };
            char *end = NULL;
            long v = strtol(hex, &end, 16);
            if (end == hex + 2) { s[wr++] = (char)v; rd += 3; }
            else { s[wr++] = s[rd++]; }
        } else {
            s[wr++] = s[rd++];
        }
    }
    s[wr] = '\0';
    return wr;
}

/* 路径安全校验：禁止 .. 越界、必须以已知前缀开头 */
static bool path_is_safe(const char *rel_path)
{
    if (!rel_path || !rel_path[0]) return false;
    if (strstr(rel_path, "..")) return false;
    if (strstr(rel_path, "//")) return false;
    if (strncmp(rel_path, "scenes/", 7) == 0) return true;
    if (strncmp(rel_path, "status/", 7) == 0) return true;
    if (strncmp(rel_path, "asset/", 6) == 0)  return true;
    return false;
}

/* ================================================================
 *  Handler: GET /behavior — 返回编辑页 HTML
 * ================================================================ */
static esp_err_t beh_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req,
        (const char *)behavior_html_start,
        (ssize_t)(behavior_html_end - behavior_html_start));
}

/* ================================================================
 *  Handler: GET /api/behavior/list — 列目录
 *  ?type=scenes|status|asset&channel=eye|led|motor|servo|audio
 * ================================================================ */
static esp_err_t beh_list_handler(httpd_req_t *req)
{
    if (!s_inited) bufs_init();
    if (!s_resp_buf) { send_json_err(req, "oom"); return ESP_FAIL; }

    char query[128] = {0};
    char type[16]   = {0};
    char channel[16] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        send_json_err(req, "missing query");
        return ESP_FAIL;
    }
    httpd_query_key_value(query, "type", type, sizeof(type));
    if (httpd_query_key_value(query, "channel", channel, sizeof(channel)) != ESP_OK) {
        channel[0] = '\0';
    }
    url_decode_inplace(type);
    url_decode_inplace(channel);

    /* 拼目录相对路径 */
    char dir_rel[64];
    if (strcmp(type, "scenes") == 0) {
        snprintf(dir_rel, sizeof(dir_rel), "scenes");
    } else if (strcmp(type, "status") == 0) {
        snprintf(dir_rel, sizeof(dir_rel), "status");
    } else if (strcmp(type, "asset") == 0) {
        if (channel[0] == '\0') { send_json_err(req, "missing channel"); return ESP_FAIL; }
        snprintf(dir_rel, sizeof(dir_rel), "asset/%s", channel);
    } else {
        send_json_err(req, "bad type");
        return ESP_FAIL;
    }

    char dir_full[96];
    snprintf(dir_full, sizeof(dir_full), "%s/%s", BEHAVIOR_FS_BASE, dir_rel);

    DIR *d = opendir(dir_full);
    if (!d) {
        ESP_LOGW(TAG, "opendir %s failed", dir_full);
        send_json_err(req, "dir not found");
        return ESP_FAIL;
    }

    memset(s_resp_buf, 0, BEH_RESP_BUF_SIZE);
    int pos = 0;
    safe_append(s_resp_buf, BEH_RESP_BUF_SIZE, &pos,
                "{\"status\":\"ok\",\"type\":\"%s\",\"channel\":\"%s\",\"files\":[",
                type, channel);

    bool first = true;
    int count = 0;

    /* 场景按类别存放于 scenes/emotion、scenes/action 等子目录，需递归列出。
     * 返回相对路径（相对 scenes/ 目录，如 "emotion/happy.json"），
     * 前端据此拼接为 scenes/emotion/happy.json。其余类型保持一层扁平文件名。 */
    bool recursive = (strcmp(type, "scenes") == 0);

    /* 用显式栈实现目录递归遍历。
     * 路径拼接使用不会触发 -Wformat-truncation / -Wstringop-truncation 的辅助函数。 */
    char stack_rel[16][128];
    char stack_path[16][192];
    int sp = 0;
    safe_strcpy(stack_rel[0], sizeof(stack_rel[0]), "");
    safe_strcpy(stack_path[0], sizeof(stack_path[0]), dir_full);
    sp++;

    while (sp > 0) {
        sp--;
        char *cur_rel = stack_rel[sp];
        char *cur_path = stack_path[sp];

        DIR *sd = opendir(cur_path);
        if (!sd) continue;
        struct dirent *sde;
        while ((sde = readdir(sd)) != NULL) {
            const char *nm = sde->d_name;
            if (nm[0] == '.') continue;
            if (strstr(nm, ".tmp")) continue;

            char child_path[192];
            safe_strcpy(child_path, sizeof(child_path), cur_path);
            path_append(child_path, sizeof(child_path), nm);

            struct stat st;
            if (recursive && stat(child_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                if (sp < (int)(sizeof(stack_rel)/sizeof(stack_rel[0]))) {
                    /* 子目录加入遍历栈（不立即输出，仅递归其内容） */
                    char child_rel[128];
                    safe_strcpy(child_rel, sizeof(child_rel), cur_rel);
                    path_append(child_rel, sizeof(child_rel), nm);
                    safe_strcpy(stack_rel[sp], sizeof(stack_rel[sp]), child_rel);
                    safe_strcpy(stack_path[sp], sizeof(stack_path[sp]), child_path);
                    sp++;
                }
                continue;
            }
            if (recursive && strstr(nm, ".json") == NULL) continue; /* 仅列举 .json */

            char entry[192];
            safe_strcpy(entry, sizeof(entry), recursive ? cur_rel : "");
            path_append(entry, sizeof(entry), nm);

            if (!first) s_resp_buf[pos++] = ',';
            safe_append(s_resp_buf, BEH_RESP_BUF_SIZE, &pos, "\"%s\"", entry);
            first = false;
            count++;
        }
        closedir(sd);
    }
    closedir(d);

    safe_append(s_resp_buf, BEH_RESP_BUF_SIZE, &pos, "]}");

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, s_resp_buf, pos);

    ESP_LOGD(TAG, "list %s: %d files", dir_rel, count);
    return ESP_OK;
}

/* ================================================================
 *  Handler: GET /api/behavior/file — 取文件原文
 *  ?path=<rel_path>
 *
 *  用 app_behavior_fs_read_text 取一份独立 PSRAM 拷贝（命中缓存则零 flash I/O），
 *  发送完即释放，避免与并发 save 的缓存指针竞争。
 * ================================================================ */
static esp_err_t beh_file_get_handler(httpd_req_t *req)
{
    char query[256] = {0};
    char path[192]  = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        send_json_err(req, "missing query");
        return ESP_FAIL;
    }
    if (httpd_query_key_value(query, "path", path, sizeof(path)) != ESP_OK) {
        send_json_err(req, "missing path");
        return ESP_FAIL;
    }
    url_decode_inplace(path);   /* %2F → / 等，否则 path_is_safe 全部拒绝 */
    if (!path_is_safe(path)) {
        send_json_err(req, "bad path");
        return ESP_FAIL;
    }

    size_t len = 0;
    char *buf = app_behavior_fs_read_text(path, &len);   /* 独立 PSRAM 拷贝，调用方释放 */
    if (!buf) {
        send_json_err(req, "file not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t r = httpd_resp_send(req, buf, len);
    heap_caps_free(buf);
    return r;
}

/* ================================================================
 *  Handler: POST /api/behavior/file — 保存/删除
 *
 *  body:
 *    {"action":"save","path":"...","content":{...}}
 *    {"action":"delete","path":"..."}
 *
 *  save 时 content 为 JSON 对象（前端先 JSON.parse 校验），
 *  C 侧取 content 子树再 cJSON_PrintUnformatted 规范化存储。
 * ================================================================ */
static esp_err_t beh_file_post_handler(httpd_req_t *req)
{
    if (!s_inited) bufs_init();
    if (!s_body_buf) { send_json_err(req, "oom"); return ESP_FAIL; }

    /* 循环读取 body（可能分段） */
    size_t total = 0;
    size_t need = req->content_len;
    if (need == 0) {
        send_json_err(req, "empty body");
        return ESP_FAIL;
    }
    if (need >= BEH_BODY_BUF_SIZE) {
        send_json_err(req, "body too large");
        return ESP_FAIL;
    }
    while (total < need) {
        int got = httpd_req_recv(req, s_body_buf + total, need - total);
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
            send_json_err(req, "recv failed");
            return ESP_FAIL;
        }
        total += got;
    }
    s_body_buf[total] = '\0';

    cJSON *root = cJSON_ParseWithLength(s_body_buf, total);
    if (!root) { send_json_err(req, "bad json"); return ESP_FAIL; }

    const cJSON *jaction = cJSON_GetObjectItemCaseSensitive(root, "action");
    const cJSON *jpath   = cJSON_GetObjectItemCaseSensitive(root, "path");
    if (!cJSON_IsString(jaction) || !jaction->valuestring ||
        !cJSON_IsString(jpath)   || !jpath->valuestring) {
        cJSON_Delete(root);
        send_json_err(req, "missing action/path");
        return ESP_FAIL;
    }
    const char *action = jaction->valuestring;
    const char *path   = jpath->valuestring;

    if (!path_is_safe(path)) {
        cJSON_Delete(root);
        send_json_err(req, "bad path");
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;
    const char *errmsg = "fail";

    if (strcmp(action, "save") == 0) {
        const cJSON *jcontent = cJSON_GetObjectItemCaseSensitive(root, "content");
        if (!jcontent) {
            cJSON_Delete(root);
            send_json_err(req, "missing content");
            return ESP_FAIL;
        }
        /* 序列化 content 子树为规范文本 */
        char *text = cJSON_PrintUnformatted(jcontent);
        if (!text) {
            cJSON_Delete(root);
            send_json_err(req, "serialize failed");
            return ESP_FAIL;
        }
        ret = app_behavior_fs_save_text(path, text, strlen(text));
        cJSON_free(text);
        if (ret != ESP_OK) errmsg = "save failed";
    } else if (strcmp(action, "delete") == 0) {
        ret = app_behavior_fs_remove(path);
        if (ret != ESP_OK) errmsg = "delete failed";
    } else {
        cJSON_Delete(root);
        send_json_err(req, "unknown action");
        return ESP_FAIL;
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "%s '%s'", action, path);
        /* 保存/删除成功后必须失效 app_behavior 的【解析结果】缓存：
         *   - 文本缓存已由 save_text(cache_put) / remove(cache_drop) 同步，无需全量 preload；
         *   - 但 L1 asset play_data / L2 scene 时间轴是解析产物，只刷新文本不会更新它们，
         *     这正是"网页保存后执行仍是旧数据"的根因。
         * 定向失效仅处理被改动的那一个文件，代价远小于全盘 preload。 */
        app_behavior_invalidate_cache(path);
        cJSON_Delete(root);
        send_json_ok(req, NULL);
        return ESP_OK;
    }
    cJSON_Delete(root);
    send_json_err(req, errmsg);
    return ESP_FAIL;
}

/* ================================================================
 *  Handler: POST /api/behavior/play — 执行行为 JSON 文件
 *
 *  body:
 *    {"path":"scenes/happy.json"}
 *    {"path":"status/status_idle.json"}
 *
 *  根据路径前缀 scenes/ 或 status/ 分别调用场景或状态播放。
 * ================================================================ */
static esp_err_t beh_play_handler(httpd_req_t *req)
{
    if (!s_inited) bufs_init();
    if (!s_body_buf) { send_json_err(req, "oom"); return ESP_FAIL; }

    /* 读取 body */
    size_t total = 0;
    size_t need = req->content_len;
    if (need == 0) {
        send_json_err(req, "empty body");
        return ESP_FAIL;
    }
    if (need >= BEH_BODY_BUF_SIZE) {
        send_json_err(req, "body too large");
        return ESP_FAIL;
    }
    while (total < need) {
        int got = httpd_req_recv(req, s_body_buf + total, need - total);
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
            send_json_err(req, "recv failed");
            return ESP_FAIL;
        }
        total += got;
    }
    s_body_buf[total] = '\0';

    cJSON *root = cJSON_ParseWithLength(s_body_buf, total);
    if (!root) { send_json_err(req, "bad json"); return ESP_FAIL; }

    const cJSON *jpath = cJSON_GetObjectItemCaseSensitive(root, "path");
    if (!cJSON_IsString(jpath) || !jpath->valuestring) {
        cJSON_Delete(root);
        send_json_err(req, "missing path");
        return ESP_FAIL;
    }
    const char *path = jpath->valuestring;

    if (!path_is_safe(path)) {
        cJSON_Delete(root);
        send_json_err(req, "bad path");
        return ESP_FAIL;
    }

    /* 从路径中提取文件名（去掉目录前缀和 .json 后缀） */
    const char *slash = strrchr(path, '/');
    const char *fname = slash ? (slash + 1) : path;

    char name[64];
    const char *dot = strrchr(fname, '.');
    if (dot) {
        size_t nlen = (size_t)(dot - fname);
        if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
        memcpy(name, fname, nlen);
        name[nlen] = '\0';
    } else {
        snprintf(name, sizeof(name), "%s", fname);
    }

    bool ok = false;
    if (strncmp(path, "scenes/", 7) == 0) {
        /* 按完整相对路径播放：不再退化成 basename 走 scene_path 的前缀猜测，
         * 否则 emotion/action/sys 下同名文件会命中错误的那一个（"执行的不是刚编辑的 JSON"）。 */
        ok = app_behavior_play_scene_path(path, NULL, BEHAVIOR_PRIO_DEFAULT);
    } else if (strncmp(path, "status/", 7) == 0) {
        ok = app_behavior_play_status(name);
    } else if (strncmp(path, "asset/", 6) == 0) {
        /* asset/<channel>/<file>.json → 包装为临时 scene 播放 */
        const char *channel_start = path + 6;           /* "eye/happy.json" */
        char channel[16];
        const char *ch_slash = strchr(channel_start, '/');
        if (!ch_slash) {
            cJSON_Delete(root);
            send_json_err(req, "bad asset path");
            return ESP_FAIL;
        }
        size_t ch_len = (size_t)(ch_slash - channel_start);
        if (ch_len >= sizeof(channel)) ch_len = sizeof(channel) - 1;
        memcpy(channel, channel_start, ch_len);
        channel[ch_len] = '\0';

        /* 构造临时 scene 名: __play_<channel>_<name>
         * channel ≤15，name 限 48 字节，加上 "__play_"(7) + '_'(1) + '\0'(1) ≤ 72 */
        char tmp_scene[80];
        char name_short[49];
        snprintf(name_short, sizeof(name_short), "%.48s", name);
        snprintf(tmp_scene, sizeof(tmp_scene), "__play_%.15s_%.48s", channel, name_short);

        char tmp_path[128];
        snprintf(tmp_path, sizeof(tmp_path), "scenes/%s.json", tmp_scene);

        /* 构造包装 scene JSON */
        cJSON *wrap = cJSON_CreateObject();
        cJSON_AddStringToObject(wrap, "scene_name", tmp_scene);
        cJSON_AddStringToObject(wrap, "priority", "normal");
        cJSON *presets = cJSON_AddArrayToObject(wrap, "presets");
        cJSON *v = cJSON_CreateObject();
        cJSON_AddStringToObject(v, "name", "v1");
        cJSON_AddNumberToObject(v, "weight", 1);
        cJSON *timeline = cJSON_AddArrayToObject(v, "timeline");
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "time_ms", 0);
        cJSON_AddStringToObject(entry, "type", channel);
        cJSON_AddNumberToObject(entry, "repeat", 1);
        cJSON_AddStringToObject(entry, "asset", fname);
        cJSON_AddItemToArray(timeline, entry);
        cJSON_AddItemToArray(presets, v);

        /* 保存临时 scene、按路径播放、清理。
         * 播放前先失效该临时场景的 L2 条目：tmp 场景名是确定性的
         * （__play_<ch>_<name>），上次播放留下的缓存条目会借用旧的 play_data，
         * 直接命中就会播到编辑前的旧关键帧。 */
        esp_err_t er = app_behavior_fs_save_json(tmp_path, wrap);
        cJSON_Delete(wrap);
        if (er == ESP_OK) {
            app_behavior_invalidate_cache(tmp_path);
            ok = app_behavior_play_scene_path(tmp_path, NULL, BEHAVIOR_PRIO_DEFAULT);
            app_behavior_fs_remove(tmp_path);
        }
        if (!ok) {
            app_behavior_fs_remove(tmp_path);
        }
    } else {
        cJSON_Delete(root);
        send_json_err(req, "only scenes/ or status/ playable");
        return ESP_FAIL;
    }

    if (ok) {
        ESP_LOGI(TAG, "play '%s' -> %s", path, name);
        cJSON_Delete(root);
        send_json_ok(req, NULL);
        return ESP_OK;
    }
    cJSON_Delete(root);
    send_json_err(req, "play failed");
    return ESP_FAIL;
}

/* ================================================================
 *  路由表
 * ================================================================ */
static const web_route_t s_routes[] = {
    { "/behavior",           HTTP_GET,  beh_page_handler,      false },
    { "/api/behavior/list",   HTTP_GET,  beh_list_handler,      false },
    { "/api/behavior/file",   HTTP_GET,  beh_file_get_handler,  false },
    { "/api/behavior/file",   HTTP_POST, beh_file_post_handler, false },
    { "/api/behavior/play",   HTTP_POST, beh_play_handler,      false },
};

void app_web_behavior_init(void)
{
    if (!s_inited) bufs_init();
    app_web_register_routes(WEB_SERVER_CTRL, s_routes,
                            sizeof(s_routes) / sizeof(s_routes[0]));
    ESP_LOGI(TAG, "behavior editor routes registered (%d)",
             (int)(sizeof(s_routes) / sizeof(s_routes[0])));
}
