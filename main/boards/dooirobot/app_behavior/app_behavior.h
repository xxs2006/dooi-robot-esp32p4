#ifndef APP_BEHAVIOR_H_
#define APP_BEHAVIOR_H_

#include "app_behavior_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化行为管理器，创建行为任务与命令队列。
 */
void app_behavior_init(void);

/**
 * @brief 播放一个场景。
 * @param scene_name  场景名（对应 resources/scenes/<name>.json）
 * @param preset     指定变体名；NULL 表示按 weight 加权随机
 * @param prio_override 优先级覆盖；传 BEHAVIOR_PRIO_DEFAULT 表示采用 JSON 中的 priority
 * @return true 成功入队
 */
bool app_behavior_play_scene(const char *scene_name, const char *preset,
                             action_priority_t prio_override);

/**
 * @brief 按资源的【完整相对路径】播放场景（绕过场景名 → 路径的猜测解析）。
 *
 * 网页编辑器拿到的是 "scenes/emotion/emotion_happy.json" 这类完整路径，
 * 若退化成文件名再走 app_behavior_fs_scene_path() 的前缀猜测，
 * 在 emotion/action/sys 下出现同名文件时会命中错误的那一个。
 *
 * @param rel_path 相对 /resources 的路径，如 "scenes/emotion/emotion_happy.json"
 * @param preset   指定变体名；NULL 表示按 weight 加权随机
 * @param prio_override 优先级覆盖；BEHAVIOR_PRIO_DEFAULT 表示采用 JSON 中的 priority
 * @return true 成功入队
 */
bool app_behavior_play_scene_path(const char *rel_path, const char *preset,
                                  action_priority_t prio_override);

/**
 * @brief 使资源热更新（网页编辑器保存/删除）立即生效。
 *
 * app_behavior 内部有两级【解析结果】缓存，均派生自 /resources 下的 JSON：
 *   L1 asset：按 "<channel>/<file>.json" 缓存 channel->parse() 的 play_data（只插入、不替换）
 *   L2 scene：按 "<scene>|<preset>" 缓存解析好的时间轴
 * 只刷新文本缓存（app_behavior_fs_preload）不足以让播放用上新数据，必须同时失效这两级。
 *
 * @param rel_path 变更的资源相对路径（"asset/eye/x.json" / "scenes/emotion/x.json" /
 *                 "status/x.json"）。传 NULL 或空串表示全量失效。
 */
void app_behavior_invalidate_cache(const char *rel_path);

/**
 * @brief 播放一个状态（PRIO_IDLE 基线，用于结束后恢复硬件）。
 * @param status_name 状态名（对应 resources/status/<name>.json）
 */
bool app_behavior_play_status(const char *status_name);

/* ==================== AI 即时编排（内联 JSON） ====================
 *
 * 与 app_behavior_play_scene() 的区别：
 *   - 不读取资源分区里的场景文件，动作由调用方（AI / Web）以 JSON 文本即时给出；
 *   - 时间轴条目允许"内联关键帧"：每个条目自带 keyframes，无需预先落盘成 asset 文件；
 *   - 因此不查、也不写入 L1/L2 解析缓存（一次性动作入缓存只会挤占常驻资源），
 *     各通道 play_data 为该播放体自有，结束即 destroy 回收。
 */

/** 内联 JSON 文本长度上限（字节）：超限直接拒绝，避免异常输入打爆内存 */
#define BEHAVIOR_INLINE_JSON_MAX   8192

/** 未显式指定场景名时使用的默认名 */
#define BEHAVIOR_INLINE_SCENE_NAME "ai_compose"

/** 内联编排的执行结果（供调用方向上层/AI 反馈） */
typedef struct {
    bool     ok;              /* 是否成功提交播放 */
    int      item_total;      /* timeline 中提交的条目总数 */
    int      item_count;      /* 其中解析成功的条目数 */
    int      keyframe_total;  /* 成功条目的关键帧总数 */
    uint32_t duration_ms;     /* 预估总时长(ms)；0 = 含 repeat=0 无限项 */
    /* 失败原因（ok=false 时有效）。中文按 UTF-8 3 字节/字计，取 192 留足余量，
     * 避免各分支的提示文案触发 -Werror=format-truncation。 */
    char     error[192];
} behavior_inline_result_t;

/**
 * @brief 用一段即时 JSON 编排并播放一个场景动作。
 *
 * 接受的两种顶层结构：
 *   A) { "priority":"normal", "timeline":[ ... ] }
 *   B) { "priority":"normal", "presets":[ { "name":"p", "timeline":[ ... ] } ] }
 *      （B 形式只取第一个 preset 的 timeline）
 *
 * timeline 条目字段：
 *   time_ms   : 相对动作开始的延时(ms)
 *   type      : 通道名 eye / iris / led / servo / motor / audio（也可用 channel 别名）
 *   repeat    : 播放遍数，0 = 无限循环（只能被后续更高优先级行为打断）
 *   asset     : 可选。为字符串时按资源名解析（复用已有资产）；为对象时把它当作 asset 直接 parse
 *   keyframes : 内联关键帧数组（各通道字段见对应 behavior_xx_parse.c）
 * 未写 asset 时，条目自身会被当作 asset 交给通道 parse（iris 等无 keyframes 的通道走此路径）。
 *
 * @param scene_name    场景名（记录/调试用，最长 BEHAVIOR_NAME_MAX-1）；传 NULL 用默认名
 * @param json_text     以 '\0' 结尾的 JSON 文本（长度 <= BEHAVIOR_INLINE_JSON_MAX）
 * @param prio_override 优先级覆盖；BEHAVIOR_PRIO_DEFAULT 表示采用 JSON 中的 priority
 * @param result        可选，输出执行结果；失败时 error 为可读的中文原因
 * @return true 已提交播放
 */
bool app_behavior_play_scene_json(const char *scene_name, const char *json_text,
                                  action_priority_t prio_override,
                                  behavior_inline_result_t *result);

/** @brief 停止优先级 >= drop_below 的所有行为（含排队中的） */
bool app_behavior_stop(action_priority_t drop_below);

bool app_behavior_is_idle(void);
const char *app_behavior_current_name(void);
action_priority_t app_behavior_current_priority(void);

/* ==================== 内部（供 app_behavior_status 使用） ==================== */
typedef void (*behavior_scene_ended_hook_t)(const behavior_result_t *result,
                                            const char *restore_status);
void app_behavior_set_scene_ended_hook(behavior_scene_ended_hook_t fn);

/* scene 开始播放时回调（在 start_playback 中、status 之外触发），
 * 供状态模块记录"进入 scene 前的当前状态"，以便 scene 结束后恢复。 */
typedef void (*behavior_scene_started_hook_t)(const char *scene_name,
                                              const char *preset);
void app_behavior_set_scene_started_hook(behavior_scene_started_hook_t fn);

#ifdef __cplusplus
}
#endif

#endif /* APP_BEHAVIOR_H_ */