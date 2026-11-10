#include "app_mcp_tool.h"
#include "mcp_server.h"
#include "app_behavior.h"
#include "app_behavior_fs.h"
#include "app_ble_control.h"
#include "eyes_control.h"
#include "app_ui.h"
#include "app_ui_logic.h"
#include "dooi_display.h"
#include "app_servo.h"
#include "app_motor.h"
#include "app_periphral.h"
#include <string>
#include <map>
#include <cstdlib>
#include <cstring>
#include "application.h"
#include "esp_log.h"
#include <cJSON.h>

// 场景动作 JSON 所在目录（相对 /resources），AI 可通过 list 动态读取其 scene_name / description
#define MCP_ACTION_SCENES_DIR  "scenes/action"

// 从缓存文本的 JSON 中轻量提取一个顶层字符串字段的值（不含引号）。
// 不做完整 cJSON 解析，直接定位 "key" : "value"，速度快、零额外内存。
static bool extract_json_string(const char *text, const char *key, char *out, size_t outsz)
{
    if (!text || !key || !out) return false;
    char pat[64];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(text, pat);
    if (!p) return false;
    p += n;
    // 跳过空白与冒号
    while (*p && (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outsz) {
        if (*p == '\\' && p[1]) { p++; }  // 跳过转义（粗略处理）
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

struct scene_list_ctx {
    std::string *out;
};

// 遍历缓存中的 action 场景，动态构建给 AI 的描述文本（仅读 RAM 缓存，不触 SPIFFS）。
// 返回形如：
//   action_wave_goodbye  - 挥手告别场景：笑眼+橙呼吸+挥手+问候声
//   action_dance         - 跳舞场景：绿呼吸+双臂左右摆
static bool scene_list_cb(const char *rel_path, void *arg)
{
    struct scene_list_ctx *ctx = (struct scene_list_ctx *)arg;
    const char *text = NULL;
    size_t len = 0;
    if (!app_behavior_fs_get_cached(rel_path, &text, &len) || !text) return true;  // 继续下一个

    char sn[128] = {0}, ds[256] = {0};
    extract_json_string(text, "scene_name", sn, sizeof(sn));
    extract_json_string(text, "description", ds, sizeof(ds));
    if (sn[0] == '\0') {
        // 回退：用文件名（去掉 scenes/action/ 前缀和 .json）
        const char *base = strrchr(rel_path, '/');
        snprintf(sn, sizeof(sn), "%s", base ? base + 1 : rel_path);
        size_t L = strlen(sn);
        if (L >= 5 && strcmp(sn + L - 5, ".json") == 0) sn[L - 5] = '\0';
    }

    *ctx->out += sn;
    *ctx->out += "  -  ";
    *ctx->out += ds;
    *ctx->out += "\n";
    return true;  // 继续
}

static std::string build_action_scene_list(void)
{
    // 确保资源分区已挂载、缓存已预取（首次调用兜底；app_behavior_fs_init 幂等且会预加载缓存）
    if (!app_behavior_fs_mounted()) {
        app_behavior_fs_init();
    }
    std::string out;
    struct scene_list_ctx ctx = { &out };
    app_behavior_fs_foreach_cached(MCP_ACTION_SCENES_DIR "/", scene_list_cb, &ctx);
    return out;
}

// ==================== compose 简化编排（自封装为完整 timeline JSON） ====================
//
// 目的：AI 只需给出"高层意图"，本模块将其二次封装成行为引擎要求的完整 JSON
// （type/time_ms/repeat/keyframes/duration_ms 等字段由本模块补齐），大幅减少
// AI 生成 token 与推理耗时。
//
// 简化 spec（字段全部可选，缺省自动填充）：
//   {
//     "name":    "my_action",          // 可选，动作名
//     "priority":"normal",             // 可选：idle|low|normal|high|critical
//     "delay_ms": 200,                 // 可选，各部件相对动作开始的整体延时
//     "eye":   "happy",                // 眼睛：字符串=表情；或 {"expr":"happy","ms":2000}
//     "iris":  "emerald",              // 虹膜颜色：default|emerald|violet|blue|amber|gray|ruby|winered
//     "led":   "rainbow",              // 灯光：字符串=模式；或 {"mode":"solid","color":"red","period":1000}
//     "servo": [{"l":20,"r":160,"ms":300},{"l":160,"r":20,"ms":300}],  // 双臂关键帧，末尾自动归位90/90
//     "motor": {"forward":60,"ms":800}, // 移动：forward/back/left/right 或 {"l":..,"r":..}；末尾自动停止
//     "audio": "laugh"                 // 音效：happy|laugh|sad|angry|surprised|wow|tick
//   }

// ---- 轻量 cJSON 取值工具 ----
static bool spec_get_string(const cJSON *obj, const char *key, std::string *out)
{
    if (!obj || !key || !out) return false;
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it || !cJSON_IsString(it) || !it->valuestring) return false;
    *out = it->valuestring;
    return true;
}

static bool spec_get_int(const cJSON *obj, const char *key, int def, int *out)
{
    if (!obj || !key || !out) return false;
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it || !cJSON_IsNumber(it)) { *out = def; return true; }
    *out = it->valueint;
    return true;
}

// 把字符串或 {mode,color,period} 统一成灯光关键帧对象
static void led_emit(cJSON *timeline, int delay_ms, const cJSON *led)
{
    if (!led) return;
    std::string mode = "off";
    std::string color;
    int period = 1000;
    if (cJSON_IsString(led)) {
        mode = led->valuestring;
    } else if (cJSON_IsObject(led)) {
        spec_get_string(led, "mode", &mode);
        spec_get_string(led, "color", &color);
        spec_get_int(led, "period", 1000, &period);
    }
    if (mode == "off") return;

    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "time_ms", delay_ms);
    cJSON_AddStringToObject(item, "type", "led");
    cJSON_AddNumberToObject(item, "repeat", 0);   // 灯效作背景，无限循环直至动作结束
    cJSON *kfs = cJSON_CreateArray();
    cJSON *kf0 = cJSON_CreateObject();
    cJSON_AddNumberToObject(kf0, "time_ms", 0);
    cJSON_AddStringToObject(kf0, "mode", mode.c_str());
    cJSON_AddNumberToObject(kf0, "period_ms", period);
    cJSON_AddBoolToObject(kf0, "symmetric", 1);
    if (!color.empty()) cJSON_AddStringToObject(kf0, "color", color.c_str());
    cJSON_AddItemToArray(kfs, kf0);
    cJSON_AddItemToObject(item, "keyframes", kfs);
    cJSON_AddItemToArray(timeline, item);
}

// 眼睛：字符串=表情，或 {expr,ms}，或 [ {move_x,move_y,ms}, ... ] 视线扫视
static void eye_emit(cJSON *timeline, int delay_ms, const cJSON *eye)
{
    if (!eye) return;

    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "time_ms", delay_ms);
    cJSON_AddStringToObject(item, "type", "eye");
    cJSON_AddNumberToObject(item, "repeat", 1);
    cJSON *kfs = cJSON_CreateArray();

    if (cJSON_IsArray(eye)) {
        // 视线扫视：{move_x,move_y,ms} 关键帧序列，ms 可选（默认 400）
        int n = cJSON_GetArraySize(eye);
        int t = 0;
        for (int i = 0; i < n; i++) {
            const cJSON *kf = cJSON_GetArrayItem(eye, i);
            if (!cJSON_IsObject(kf)) continue;
            cJSON *jf = cJSON_CreateObject();
            const cJSON *mx = cJSON_GetObjectItemCaseSensitive(kf, "move_x");
            const cJSON *my = cJSON_GetObjectItemCaseSensitive(kf, "move_y");
            if (!cJSON_IsNumber(mx) && !cJSON_IsNumber(my)) { cJSON_Delete(jf); continue; }
            int dms = 400;
            spec_get_int(kf, "ms", 400, &dms);
            cJSON_AddNumberToObject(jf, "time_ms", t);
            if (cJSON_IsNumber(mx)) cJSON_AddNumberToObject(jf, "move_x", mx->valuedouble);
            if (cJSON_IsNumber(my)) cJSON_AddNumberToObject(jf, "move_y", my->valuedouble);
            cJSON_AddNumberToObject(jf, "duration_ms", dms);
            cJSON_AddItemToArray(kfs, jf);
            t += (dms > 0 ? dms : 1);
        }
        if (cJSON_GetArraySize(kfs) > 0) {
            cJSON_AddItemToObject(item, "keyframes", kfs);
            cJSON_AddItemToArray(timeline, item);
        } else {
            cJSON_Delete(kfs);
            cJSON_Delete(item);
        }
        return;
    }

    std::string expr;
    int ms = 1500;
    if (cJSON_IsString(eye)) {
        expr = eye->valuestring;
    } else if (cJSON_IsObject(eye)) {
        spec_get_string(eye, "expr", &expr);
        spec_get_int(eye, "ms", 1500, &ms);
    }
    if (expr.empty()) { cJSON_Delete(item); return; }

    cJSON *kf0 = cJSON_CreateObject();
    cJSON_AddNumberToObject(kf0, "time_ms", 0);
    cJSON_AddStringToObject(kf0, "expr", expr.c_str());
    cJSON_AddNumberToObject(kf0, "duration_ms", ms);
    cJSON_AddItemToArray(kfs, kf0);
    cJSON_AddItemToObject(item, "keyframes", kfs);
    cJSON_AddItemToArray(timeline, item);
}

static void iris_emit(cJSON *timeline, int delay_ms, const cJSON *iris)
{
    if (!iris || !cJSON_IsString(iris)) return;
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "time_ms", delay_ms);
    cJSON_AddStringToObject(item, "type", "iris");
    cJSON_AddNumberToObject(item, "repeat", 1);
    cJSON_AddStringToObject(item, "iris", iris->valuestring);
    cJSON_AddItemToArray(timeline, item);
}

// 双臂：关键帧数组 {l,r,ms}，末尾自动追加归位(90/90)
static void servo_emit(cJSON *timeline, int delay_ms, const cJSON *servo)
{
    if (!cJSON_IsArray(servo)) return;
    int n = cJSON_GetArraySize(servo);
    if (n <= 0) return;

    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "time_ms", delay_ms);
    cJSON_AddStringToObject(item, "type", "servo");
    cJSON_AddNumberToObject(item, "repeat", 1);
    cJSON *kfs = cJSON_CreateArray();
    int t = 0;
    for (int i = 0; i < n; i++) {
        const cJSON *kf = cJSON_GetArrayItem(servo, i);
        if (!cJSON_IsObject(kf)) continue;
        int l = 90, r = 90, ms = 400;
        spec_get_int(kf, "l", 90, &l);
        spec_get_int(kf, "r", 90, &r);
        spec_get_int(kf, "ms", 400, &ms);
        cJSON *jf = cJSON_CreateObject();
        cJSON_AddNumberToObject(jf, "time_ms", t);
        cJSON_AddNumberToObject(jf, "left_arm", l);
        cJSON_AddNumberToObject(jf, "right_arm", r);
        cJSON_AddNumberToObject(jf, "duration_ms", ms);
        cJSON_AddItemToArray(kfs, jf);
        t += (ms > 0 ? ms : 1);
    }
    // 自动归位
    cJSON *home = cJSON_CreateObject();
    cJSON_AddNumberToObject(home, "time_ms", t);
    cJSON_AddNumberToObject(home, "left_arm", 90);
    cJSON_AddNumberToObject(home, "right_arm", 90);
    cJSON_AddNumberToObject(home, "duration_ms", 400);
    cJSON_AddItemToArray(kfs, home);
    cJSON_AddItemToObject(item, "keyframes", kfs);
    cJSON_AddItemToArray(timeline, item);
}

// 移动：{forward:60} 前进 / {back:60} 后退 / {left:60} 左转 / {right:60} 右转
//       （数值即速度，-100~100），或 {l:..,r:..} 显式左右轮；末尾自动停止(0/0)
static void motor_emit(cJSON *timeline, int delay_ms, const cJSON *motor)
{
    if (!motor || !cJSON_IsObject(motor)) return;
    int l = 0, r = 0, ms = 800;
    bool has = false;
    const cJSON *dir = NULL;
    if ((dir = cJSON_GetObjectItemCaseSensitive(motor, "forward")) && cJSON_IsNumber(dir)) {
        l = r = dir->valueint; has = true;
    } else if ((dir = cJSON_GetObjectItemCaseSensitive(motor, "back")) && cJSON_IsNumber(dir)) {
        l = r = -dir->valueint; has = true;
    } else if ((dir = cJSON_GetObjectItemCaseSensitive(motor, "left")) && cJSON_IsNumber(dir)) {
        l = -dir->valueint; r = dir->valueint; has = true;
    } else if ((dir = cJSON_GetObjectItemCaseSensitive(motor, "right")) && cJSON_IsNumber(dir)) {
        l = dir->valueint; r = -dir->valueint; has = true;
    } else if (cJSON_GetObjectItemCaseSensitive(motor, "l") ||
               cJSON_GetObjectItemCaseSensitive(motor, "r")) {
        spec_get_int(motor, "l", 0, &l);
        spec_get_int(motor, "r", 0, &r);
        has = true;
    }
    spec_get_int(motor, "ms", 800, &ms);
    if (!has) return;

    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "time_ms", delay_ms);
    cJSON_AddStringToObject(item, "type", "motor");
    cJSON_AddNumberToObject(item, "repeat", 1);
    cJSON *kfs = cJSON_CreateArray();
    cJSON *kf0 = cJSON_CreateObject();
    cJSON_AddNumberToObject(kf0, "time_ms", 0);
    cJSON_AddNumberToObject(kf0, "speed_l", l);
    cJSON_AddNumberToObject(kf0, "speed_r", r);
    cJSON_AddNumberToObject(kf0, "duration_ms", ms);
    cJSON_AddItemToArray(kfs, kf0);
    // 自动停止
    cJSON *stop = cJSON_CreateObject();
    cJSON_AddNumberToObject(stop, "time_ms", ms);
    cJSON_AddNumberToObject(stop, "speed_l", 0);
    cJSON_AddNumberToObject(stop, "speed_r", 0);
    cJSON_AddNumberToObject(stop, "duration_ms", 0);
    cJSON_AddItemToArray(kfs, stop);
    cJSON_AddItemToObject(item, "keyframes", kfs);
    cJSON_AddItemToArray(timeline, item);
}

static void audio_emit(cJSON *timeline, int delay_ms, const cJSON *audio)
{
    if (!audio || !cJSON_IsString(audio)) return;
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "time_ms", delay_ms);
    cJSON_AddStringToObject(item, "type", "audio");
    cJSON_AddNumberToObject(item, "repeat", 1);
    cJSON *kfs = cJSON_CreateArray();
    cJSON *kf0 = cJSON_CreateObject();
    cJSON_AddNumberToObject(kf0, "time_ms", 0);
    cJSON_AddStringToObject(kf0, "clip", audio->valuestring);
    cJSON_AddItemToArray(kfs, kf0);
    cJSON_AddItemToObject(item, "keyframes", kfs);
    cJSON_AddItemToArray(timeline, item);
}

// 把简化 spec 封装成行为引擎要求的完整 timeline JSON 文本
static std::string spec_encode(const cJSON *spec)
{
    std::string name, priority = "high";
    int delay_ms = 0;
    if (cJSON_IsObject(spec)) {
        spec_get_string(spec, "name", &name);
        spec_get_string(spec, "priority", &priority);
        spec_get_int(spec, "delay_ms", 0, &delay_ms);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "priority", priority.c_str());
    cJSON *timeline = cJSON_CreateArray();

    if (cJSON_IsObject(spec)) {
        eye_emit(timeline, delay_ms, cJSON_GetObjectItemCaseSensitive(spec, "eye"));
        iris_emit(timeline, delay_ms, cJSON_GetObjectItemCaseSensitive(spec, "iris"));
        led_emit(timeline, delay_ms, cJSON_GetObjectItemCaseSensitive(spec, "led"));
        servo_emit(timeline, delay_ms, cJSON_GetObjectItemCaseSensitive(spec, "servo"));
        motor_emit(timeline, delay_ms, cJSON_GetObjectItemCaseSensitive(spec, "motor"));
        audio_emit(timeline, delay_ms, cJSON_GetObjectItemCaseSensitive(spec, "audio"));
    }

    cJSON_AddItemToObject(root, "timeline", timeline);
    char *s = cJSON_PrintUnformatted(root);
    std::string out = s ? s : "";
    if (s) cJSON_free(s);
    cJSON_Delete(root);
    (void)name;
    return out;
}

// ==================== MCP 工具注册 ====================

// 统一投递摄像头模式切换请求（视觉类工具的公共入口）
static bool camera_request_mode(camera_mode_t mode, camera_trigger_t trigger)
{
    camera_request_t req = {
        .mode    = mode,
        .trigger = trigger
    };
    camera_trigger_funtion(&req);
    return true;
}

void app_mcp_tool_register(void) {
    McpServer& mcp = McpServer::GetInstance();

    /* ============================================================
     * 分类一 · 屏幕显示（self.display.*）
     * 仅切换屏幕页面，不涉及摄像头
     * ============================================================ */

    // ──────────────────────────────────────────────────────────────
    // 工具 1 · self.display.eyes
    // ──────────────────────────────────────────────────────────────
    mcp.AddTool(
        "self.display.eyes",
        "将机器人屏幕切换到默认的眼睛 / 待机画面。无需参数。\n\n"
        "触发词：切换回正常、显示眼睛、回到待机、恢复默认表情。\n\n"
        "示例：{}",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            app_ui_logic_request_screen(SCREEN_EYES);
            return "屏幕已切换到眼睛 / 待机画面。";
        }
    );

    // ──────────────────────────────────────────────────────────────
    // 工具 2 · self.display.clock
    // ──────────────────────────────────────────────────────────────
    mcp.AddTool(
        "self.display.clock",
        "在屏幕上显示当前时间的数字时钟（时:分:秒）。无需参数。\n\n"
        "触发词：现在几点、显示时钟、查看时间、几点了。\n\n"
        "示例：{}",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            digital_clock_mode_set(MODE_TIME);
            app_ui_logic_request_screen(SCREEN_DIGITAL_CLOCK);
            return "屏幕已切换到数字时钟，显示当前时间。";
        }
    );

    // ──────────────────────────────────────────────────────────────
    // 工具 3 · self.display.pomodoro
    // ──────────────────────────────────────────────────────────────
    mcp.AddTool(
        "self.display.pomodoro",
        "在屏幕上启动番茄钟 / 专注倒计时，到点后自动结束。\n\n"
        "参数：\n"
        "  minutes : int  专注分钟数，范围 1–99，默认 25。\n\n"
        "触发词：专注计时、开始倒计时、番茄钟、倒计时 10 分钟。\n\n"
        "示例：\n"
        "  默认 25 分钟 : {}\n"
        "  专注 30 分钟 : {\"minutes\":30}",
        PropertyList({
            Property("minutes", kPropertyTypeInteger, 25, 1, 99)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            int minutes = properties["minutes"].value<int>();
            if (minutes < 1)   minutes = 1;
            if (minutes > 99)  minutes = 99;

            digital_timer_set_minutes(minutes);
            digital_clock_mode_set(MODE_TIMER);
            app_ui_logic_request_screen(SCREEN_DIGITAL_CLOCK);
            return "番茄钟已启动，时长 " + std::to_string(minutes) + " 分钟，屏幕上开始倒计时。";
        }
    );

    // ──────────────────────────────────────────────────────────────
    // 工具 4 · self.display.lucky_cat
    // ──────────────────────────────────────────────────────────────
    mcp.AddTool(
        "self.display.lucky_cat",
        "在屏幕上播放招财猫动画。\n\n"
        "参数：\n"
        "  style : int  招财猫样式，范围 1–4，默认 1。\n\n"
        "触发词：招财猫、播放动画、装饰屏幕、换个招财猫。\n\n"
        "示例：\n"
        "  默认样式 : {}\n"
        "  第 2 款  : {\"style\":2}",
        PropertyList({
            Property("style", kPropertyTypeInteger, 1, 1, 4)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            int style = properties["style"].value<int>();
            if (style < 1) style = 1;
            if (style > 4) style = 4;

            lucky_cat_style_set(style);
            app_ui_logic_request_screen(SCREEN_LUCKY_CAT);
            return "屏幕已切换到招财猫动画（样式 " + std::to_string(style) + "）。";
        }
    );

    // ──────────────────────────────────────────────────────────────
    // 工具 5 · self.display.music
    // ──────────────────────────────────────────────────────────────
    mcp.AddTool(
        "self.display.music",
        "将屏幕切换到音乐频谱可视化界面。无需参数。\n\n"
        "用户要播放音乐或点歌曲时，先切换到该界面。\n\n"
        "触发词：开始播放音乐、音乐模式、频谱显示、放首歌。\n\n"
        "示例：{}",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            app_ui_logic_request_screen(SCREEN_MUSIC_SPECTRUM);
            return "屏幕已切换到音乐频谱界面。";
        }
    );

    /* ============================================================
     * 分类二 · 摄像头视觉（self.camera.*）
     * 会打开摄像头并切换到对应视觉模式
     * ============================================================ */

    // ──────────────────────────────────────────────────────────────
    // 工具 6 · self.camera.face_track
    // ──────────────────────────────────────────────────────────────
    mcp.AddTool(
        "self.camera.face_track",
        "打开摄像头进入人脸检测模式，并持续自动追踪镜头中的人脸（云台跟随）。无需参数。\n\n"
        "触发词：追踪我的脸、人脸检测、跟随我、看着我。\n\n"
        "示例：{}",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            return camera_request_mode(CAMERA_MODE_FACE_DETECT, CAMERA_TRIGGER_FACE_TRACK_LOOP)
                ? "摄像头已开启：人脸检测 + 自动追踪中。"
                : "错误：人脸追踪请求失败。";
        }
    );

    // ──────────────────────────────────────────────────────────────
    // 工具 7 · self.camera.gesture
    // ──────────────────────────────────────────────────────────────
    mcp.AddTool(
        "self.camera.gesture",
        "打开摄像头进入手势识别模式，识别用户手势。无需参数。\n\n"
        "触发词：手势控制、识别手势、看我的手势。\n\n"
        "示例：{}",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            return camera_request_mode(CAMERA_MODE_GESTURE_RECOGNIZE, CAMERA_TRIGGER_NONE)
                ? "摄像头已开启：手势识别模式。"
                : "错误：手势识别请求失败。";
        }
    );

    /* ============================================================
     * 分类三 · 其他功能
     * ============================================================ */

    // ──────────────────────────────────────────────────────────────
    // 工具 8 · self.network.ip
    // ──────────────────────────────────────────────────────────────
    mcp.AddTool(
        "self.network.ip",
        "获取机器人当前的 Wi-Fi IP 地址，用于访问网页管理后台。"
        "IP 地址将在机器人屏幕上显示 10 秒，并在响应中返回。\n\n"
        "当用户询问以下内容时触发此工具：\n"
        "  'IP 地址是什么'、'怎么访问网页界面'、"
        "'打开后台'、'给我网址'、'网络地址'。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            char ip_buf[64] = {0};
            get_wifi_ip_string(ip_buf, sizeof(ip_buf));
            printf("[网络] ip: %s\n", ip_buf);

            // 在屏幕上显示 IP 10 秒
            screen_show_wakeup_ack("网络信息", ip_buf, APP_UI_MS_TO_TICKS(10000));

            std::string result(ip_buf);

            const char* ip_start = strstr(ip_buf, "IP: ");
            if (ip_start && strstr(ip_buf, "disconnected") == nullptr) {
                std::string pure_ip(ip_start + 4);
                result += "\n网页后台：http://" + pure_ip;
                result += "\nIP 已在屏幕上显示 10 秒。";
            } else {
                result += "\nWi-Fi 未连接，请先配置 Wi-Fi。";
            }

            return result;
        }
    );

#if 0
    // ──────────────────────────────────────────────────────────────
    // 工具 9 · self.movement.control
    //──────────────────────────────────────────────────────────────
    mcp.AddTool(
        "self.movement.control",
        "控制机器人底盘运动。支持直接设置电机速度、精确角度旋转和紧急停止。\n\n"
            "指令类型：\n"
        "  motor      – 设置两侧履带电机速度\n"
        "    left_speed  : int [-255, 255]  负值=后退\n"
        "    right_speed : int [-255, 255]  负值=后退\n"
        "    duration    : int ms（0=持续运行直到显式停止）\n"
        "    运动模式示例：\n"
        "      前进      → left= 100, right= 100\n"
        "      后退      → left=-100, right=-100\n"
        "      原地右转  → left= 100, right=-100\n"
        "      原地左转  → left=-100, right= 100\n"
        "      右弧线    → left= 100, right=  50\n\n"
        "  turn_angle – 精确旋转指定角度后自动停止\n"
        "    angle : int  正值=右转，负值=左转  [-360, 360]\n"
        "    speed : int  [1, 255]（默认 100）\n\n"
        "  stop       – 立即停止所有电机（无需其他字段）\n\n"

        "示例：\n"
        "  前进 1 秒       : {\"command\":\"motor\",\"left_speed\":100,\"right_speed\":100,\"duration\":1000}\n"
        "  后退 0.5 秒     : {\"command\":\"motor\",\"left_speed\":-80,\"right_speed\":-80,\"duration\":500}\n"
        "  右转 90°        : {\"command\":\"turn_angle\",\"angle\":90,\"speed\":80}\n"
        "  左转 180°       : {\"command\":\"turn_angle\",\"angle\":-180,\"speed\":60}\n"
        "  紧急停止        : {\"command\":\"stop\"}",
        PropertyList({
            Property("command",     kPropertyTypeString),
            Property("left_speed",  kPropertyTypeInteger,   0, -255, 255),
            Property("right_speed", kPropertyTypeInteger,   0, -255, 255),
            Property("duration",    kPropertyTypeInteger,   0,    0, 30000),
            Property("angle",       kPropertyTypeInteger,   0, -360, 360),
            Property("speed",       kPropertyTypeInteger, 100,    1, 255)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            std::string command = properties["command"].value<std::string>();
            printf("[运动控制] command: %s\n", command.c_str());

            if (command == "motor") {
                int left_speed  = properties["left_speed"].value<int>();
                int right_speed = properties["right_speed"].value<int>();
                int duration    = properties["duration"].value<int>();

                esp_err_t ret = app_motor_move_auto(
                    (int16_t)left_speed,
                    (int16_t)right_speed,
                    (uint32_t)duration);
                return (ret == ESP_OK);

            } else if (command == "turn_angle") {
                int angle = properties["angle"].value<int>();
                int speed = properties["speed"].value<int>();
                if (speed <= 0) speed = 100;

                esp_err_t ret = app_motor_turn_angle((float)angle, (int16_t)speed);
                return (ret == ESP_OK);

            } else if (command == "stop") {
                app_motor_move_auto(0, 0, 0);
                return "所有电机已停止。";

            } else {
                return "错误：未知指令 '" + command + "'";
            }
        }
    );
#endif
    // ──────────────────────────────────────────────────────────────
    // 工具 10 · self.get_msg
    // ──────────────────────────────────────────────────────────────
    mcp.AddTool(
        "self.get_msg",
        "从录音缓冲区中取出一条待处理的对话消息，用于后续对话。"
        "当需要检索缓冲的用户输入以生成跟进回复时，请使用此工具。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            return app.TapeRecorderMessage;
        }
    );

    // ──────────────────────────────────────────────────────────────
    // 工具 11 · self.open_ble
    // ──────────────────────────────────────────────────────────────
    mcp.AddTool(
        "self.open_ble",
        "启用机器人蓝牙并开始扫描附近的 BLE 设备（例如 Xbox 手柄）。"
        "若在超时时间内没有设备连接，蓝牙将自动关闭。\n\n"
        "触发词：'连接蓝牙'、'配对手柄'、'开启蓝牙'、'扫描设备'。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            app_ble_trigger_xbox_scan();
            return true;
        }
    );

    // ──────────────────────────────────────────────────────────────
    // 工具 12 · self.open_manual
    // ──────────────────────────────────────────────────────────────
    mcp.AddTool(
        "self.open_manual",
        "在屏幕上显示二维码，用户扫描后可访问用户手册或产品文档。\n\n"
        "触发词：'显示说明书'、'怎么使用'、'扫描二维码'、'操作指南'、'产品文档'。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            app_ui_logic_request_screen(SCREEN_CUSTOM_PAGE);
            return true;
        }
    );

    // ──────────────────────────────────────────────────────────────
    // 工具 13 · self.control_ir
    // ──────────────────────────────────────────────────────────────
    mcp.AddTool(
        "self.control_ir",
        "启用或禁用用于防跌落的红外（IR）悬崖检测传感器。"
        "启用后，机器人在检测到边缘或落差时会自动停止。\n\n"
        "触发词：\n"
        "  'on'  – '开启防跌落'、'打开悬崖检测'、'激活红外传感器'\n"
        "  'off' – '关闭防跌落'、'关闭悬崖传感器'、'禁用红外'\n\n"
        "参数 value 取值：\"on\" | \"off\"",
        PropertyList({
            Property("value", kPropertyTypeString)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            std::string value = properties["value"].value<std::string>();
            if (value == "on") {
                ir_feature_enable();
                return true;
            } else if (value == "off") {
                ir_feature_disable();
                return true;
            }
            return "错误：value 必须为 \"on\" 或 \"off\"";
        }
    );

    // ──────────────────────────────────────────────────────────────
    // 工具 14 · self.display.weather
    // ──────────────────────────────────────────────────────────────
    mcp.AddTool(
        "self.display.weather",
        "用户询问天气，获取到天气数据后，一定要调用此工具触发展示天气数据\n\n"

        "参数说明：\n"
        "  location    : string  – 地点名称，例如 \"深圳\"\n"
        "  condition   : string  – 天气情况，必须从以下固定值中选择：\n"
        "                  sunny          – 晴天\n"
        "                  partly_cloudy  – 多云\n"
        "                  cloudy         – 阴天\n"
        "                  overcast       – 阴霾\n"
        "                  foggy          – 雾/霾\n"
        "                  windy          – 大风\n"
        "                  drizzle        – 小雨/毛毛雨\n"
        "                  rainy          – 中雨\n"
        "                  heavy_rain     – 大雨/暴雨\n"
        "                  thunderstorm   – 雷阵雨/雷暴\n"
        "                  sleet          – 雨夹雪\n"
        "                  snowy          – 小雪/中雪\n"
        "                  heavy_snow     – 大雪/暴雪\n"
        "  temperature : int    – 当前温度（摄氏度），范围 [-60, 60]\n\n"

        "示例：\n"
        "  {\"location\":\"深圳\",\"condition\":\"sunny\",\"temperature\":28}\n",
        PropertyList({
            Property("location",    kPropertyTypeString),
            Property("condition",   kPropertyTypeString),
            Property("temperature", kPropertyTypeInteger, 20, -60, 60)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            std::string location    = properties["location"].value<std::string>();
            std::string condition   = properties["condition"].value<std::string>();
            int         temperature = properties["temperature"].value<int>();

            // 固定天气情况映射表：英文 key → 中文显示 + emoji
            static const std::map<std::string, std::pair<std::string, std::string>> s_condition_map = {
                { "sunny",         { "晴天",     "☀️"  } },
                { "partly_cloudy", { "多云",     "⛅"  } },
                { "cloudy",        { "阴天",     "☁️"  } },
                { "foggy",         { "雾/霾",    "🌁"  } },
                { "drizzle",       { "小雨",     "🌦️"  } },
                { "rainy",         { "中雨",     "🌧️"  } },
                { "heavy_rain",    { "暴雨",     "⛈️"  } },
                { "snowy",         { "小雪",     "❄️"  } },
                { "heavy_snow",    { "大雪",     "🌨️"  } },
            };

            // 校验 condition
            auto it = s_condition_map.find(condition);
            if (it == s_condition_map.end()) {
                return "错误：未知天气类型 '" + condition + "'，"
                    "请从 sunny/partly_cloudy/cloudy/overcast/foggy/windy/"
                    "drizzle/rainy/heavy_rain/thunderstorm/sleet/snowy/"
                    "heavy_snow/hail/sandstorm 中选择。";
            }

            const std::string& condition_zh = it->second.first;
            const std::string& emoji        = it->second.second;

            printf("[天气显示] location=%s, condition=%s, temperature=%d°C\n",
                location.c_str(), condition.c_str(), temperature);

            // 构建温度字符串，带正负号
            char temp_str[16];
            snprintf(temp_str, sizeof(temp_str),
                    "%s%d°C",
                    (temperature > 0 ? "+" : ""),
                    temperature);

            weather_screen_set_data(
                location.c_str(),
                condition_zh.c_str(),
                temperature
            );
            app_ui_logic_request_screen(SCREEN_WEATHER);

            // 构建返回给大模型的结构化摘要
            std::string result;
            result  = "地点：" + location + "\n";
            result += "天气：" + condition_zh + " " + emoji + "\n";
            result += "温度：" + std::string(temp_str);

            return result;
        }
    );

    // ──────────────────────────────────────────────────────────────
    // 工具 15 · self.action.list
    // ──────────────────────────────────────────────────────────────
    mcp.AddTool(
        "self.action.list",
        "动态列出机器人当前所有可用的『场景动作』。\n\n"
        "每个动作返回两项信息：\n"
        "  scene_name  – 动作的唯一标识，调用 self.action.play 时必须传入此名字\n"
        "  description – 该动作的含义与表现说明\n\n"
        "AI 在想让机器人执行某个动作（如挥手、跳舞等）时，应先调用本工具了解有哪些动作，"
        "再从返回结果中挑选合适的 scene_name 传给 self.action.play。\n"
        "该列表随 resources/scenes/action 目录下的 JSON 文件动态变化。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            std::string list = build_action_scene_list();
            if (list.empty()) {
                return "当前没有可用的场景动作（scenes/action 目录为空或资源未挂载）。";
            }
            std::string result = "可用场景动作列表（scene_name — description）：\n";
            result += list;
            return result;
        }
    );

    // ──────────────────────────────────────────────────────────────
    // 工具 16 · self.action.play
    // ──────────────────────────────────────────────────────────────
    mcp.AddTool(
        "self.action.play",
        "执行一个场景动作，让机器人播放对应的表情/灯光/动作/声音组合。\n\n"
        "scene_name 必须来自 self.action.list 返回的名字之一（例如 action_wave_goodbye）。\n"
        "variant 为可选的场景变体名，大多数场景可不传。\n\n"
        "示例：\n"
        "  挥手告别 : {\"scene_name\":\"action_wave_goodbye\"}\n"
        "  跳舞     : {\"scene_name\":\"action_dance\"}",
        PropertyList({
            Property("scene_name", kPropertyTypeString),                       // 必填
            Property("variant",    kPropertyTypeString, std::string(""))       // 可选，默认空
        }),
        [](const PropertyList& properties) -> ReturnValue {
            std::string scene_name = properties["scene_name"].value<std::string>();
            std::string variant    = properties["variant"].value<std::string>();

            ESP_LOGI("TAG", "mcp >> scene_name: %s, variant: %s", scene_name.c_str(), variant.c_str());

            if (scene_name.empty()) {
                return "错误：scene_name 不能为空，请先调用 self.action.list 获取可用动作名。";
            }

            // 优先通过逻辑层网关统一提交（做状态记录与互斥裁决）
            bool ok = app_ui_logic_request_behavior(
                scene_name.c_str(),
                variant.empty() ? nullptr : variant.c_str(),
                PRIO_NORMAL);

            // 网关在特殊（无调度）环境下可能不可用，回退到执行层直接播放
            if (!ok) {
                ok = app_behavior_play_scene(
                    scene_name.c_str(),
                    variant.empty() ? "" : variant.c_str(),
                    BEHAVIOR_PRIO_DEFAULT);
            }

            if (ok) {
                return "已触发场景动作：" + scene_name +
                       (variant.empty() ? "" : (" (变体:" + variant + ")"));
            }
            return "错误：未找到或未成功播放场景 '" + scene_name +
                   "'，请确认名字来自 self.action.list。";
        }
    );

    // ──────────────────────────────────────────────────────────────
    // 工具 17 · self.system.shutdown
    // ──────────────────────────────────────────────────────────────
    mcp.AddTool(
        "self.system.shutdown",
        "关闭机器人整机电源（关机）。\n\n"
        "【危险操作】只有在用户明确提出关机/断电要求时才调用，例如：\n"
        "  '关机吧'、'把电源关了'、'不用了，你休息吧'、'关闭机器人'、'shutdown'。\n"
        "若用户只是说'晚安'、'去睡觉'、'你先歇着'而没有明确要求断电，"
        "请改用 self.action.play 播放 action_goodnight 场景，不要调用本工具。\n\n"
        "参数 confirm 必须为 true 才会真正执行关机；"
        "未确认时只返回提示、不做任何动作，可用于向用户二次确认。\n\n"
        "执行流程：屏幕提示『系统即将关机』→ 停止当前对话与所有动作 → 约 2.5 秒后断电。"
        "关机后只能重新按电源键开机，请务必先向用户确认。\n\n"
        "示例：\n"
        "  确认关机 : {\"confirm\":true}",
        PropertyList({
            Property("confirm", kPropertyTypeBoolean, false)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            bool confirm = properties["confirm"].value<bool>();
            ESP_LOGI("TAG", "mcp >> shutdown, confirm: %d", confirm ? 1 : 0);

            if (!confirm) {
                return "尚未关机。请先向用户确认是否真的要关闭电源，"
                       "得到明确肯定答复后再以 confirm=true 调用本工具。";
            }

            if (!app_ui_logic_request_shutdown()) {
                return "错误：关机请求投递失败（系统忙或未就绪），请稍后重试。";
            }

            return "已收到关机指令：屏幕显示『系统即将关机』，"
                   "正在停止对话与动作，约 2.5 秒后切断电源。再见～";
        }
    );

    // ──────────────────────────────────────────────────────────────
    // 工具 18 · self.action.compose（AI 自主编排，完全自定义）
    //──────────────────────────────────────────────────────────────
    // AI 传 spec 自定义组合，本工具自动补齐底层细节并二次封装成完整 timeline JSON。
    mcp.AddTool(
        "self.action.compose",
        "【自主编排动作】让机器人做一段完全自定义/原创动作。\n"
        "需要原创动作（不是已有场景）时使用本工具。按需写部分通道即可，未写的通道不动：\n\n"

        "════════ spec 自定义组合 ════════\n"
        "{\n"
        "  \"name\":\"my_action\",      // 可选，动作名\n"
        "  \"delay_ms\":200,            // 可选，整体延时(ms)\n"
        "  \"eye\":\"happy\",           // 眼睛：字符串=表情；或 {\"expr\":\"happy\",\"ms\":2000}；\n"
        "                              //       或 [{\"move_x\":-0.6,\"move_y\":0.3,\"ms\":400}] 视线扫视\n"
        "                              //       表情：default|happy|happy1|happy2|angry|sad|suspicious|amazed|...\n"
        "  \"iris\":\"emerald\",        // 虹膜颜色：default|emerald|violet|blue|amber|gray|ruby|winered\n"
        "  \"led\":\"rainbow\",         // 灯光：字符串=模式；或 {\"mode\":\"solid\",\"color\":\"red\",\"period\":1000}\n"
        "                              //       模式：off|solid|breath|blink|rainbow\n"
        "  \"servo\":[{\"l\":20,\"r\":160,\"ms\":300},{\"l\":160,\"r\":20,\"ms\":300}],  // 双臂摆动，末尾自动归位90/90\n"
        "  \"motor\":{\"forward\":60,\"ms\":800},  // 移动：forward/back/left/right 后跟速度，或 {\"l\":..,\"r\":..}；末尾自动停止\n"
        "  \"audio\":\"laugh\"          // 音效：happy|laugh|sad|angry|surprised|wow|tick\n"
        "}\n"
        "示例：\n"
        "  {\"eye\":\"happy\",\"led\":{\"mode\":\"breath\",\"color\":\"green\",\"period\":700},\"audio\":\"laugh\"}\n"
        "  {\"eye\":[{\"move_x\":-0.6,\"move_y\":0.2},{\"move_x\":0.6,\"move_y\":0.2}],\"servo\":[{\"l\":30,\"r\":150,\"ms\":400}]}\n\n"

        "════════ 说明 ════════\n"
        "· time_ms/repeat/keyframes/归位/停止等底层细节由系统自动补齐，无需你操心。\n"
        "· 整段动作控制在 30 秒内；结束后舵机/灯光/电机会自动恢复待机态。",
        PropertyList({
            Property("spec", kPropertyTypeString),
            Property("name", kPropertyTypeString, std::string("ai_compose"))
        }),
        [](const PropertyList& properties) -> ReturnValue {
            std::string spec = properties["spec"].value<std::string>();
            std::string name = properties["name"].value<std::string>();

            if (spec.empty()) {
                return "错误：请传入 spec 自定义编排 JSON。";
            }

            /* 二次封装：简化 spec → 完整 timeline JSON */
            cJSON *root = cJSON_Parse(spec.c_str());
            if (!root) {
                return "错误：spec 解析失败，请检查 JSON 格式。";
            }
            std::string full = spec_encode(root);
            cJSON_Delete(root);
            if (full.empty()) {
                return "错误：编排内容为空（spec 未产出有效通道），请检查后重试。";
            }

            ESP_LOGI("TAG", "mcp >> compose spec: %s", full.c_str());

            behavior_inline_result_t res;
            bool ok = app_behavior_play_scene_json(
                name.empty() ? nullptr : name.c_str(),
                full.c_str(),
                BEHAVIOR_PRIO_DEFAULT,
                &res);

            if (!ok) {
                ESP_LOGW("TAG", "mcp >> compose failed: %s", res.error);
                return std::string("编排失败：") + res.error;
            }

            /* 编排动作已开始播放，跳过紧接着的下一次 SetEmotion，避免打断本动作 */
            DooiDisplay::ArmSkipNextEmotion();

            std::string out = "编排已开始执行：";
            out += (name.empty() ? std::string("ai_compose") : name);
            out += "\n有效部件 " + std::to_string(res.item_count) +
                   "/" + std::to_string(res.item_total) +
                   " 条，关键帧 " + std::to_string(res.keyframe_total) + " 个";
            if (res.duration_ms > 0) {
                out += "，预计时长 " + std::to_string(res.duration_ms) + " ms";
            } else {
                out += "，含无限循环条目（会持续到被下一个动作打断）";
            }
            if (res.item_count < res.item_total) {
                out += "\n注意：有 " + std::to_string(res.item_total - res.item_count) +
                       " 条未被识别，其余部件已正常执行。";
            }
            return out;
        }
    );
}