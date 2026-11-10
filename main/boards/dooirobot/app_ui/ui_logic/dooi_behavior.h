#ifndef DOOI_BEHAVIOR_H
#define DOOI_BEHAVIOR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BEHAVIOR_STATE_IDLE,
    BEHAVIOR_STATE_DEFAULT,
    BEHAVIOR_STATE_FOCUS,
    BEHAVIOR_STATE_HAPPY,
} dooi_behavior_state_t;

#define BEHAVIOR_TIMEOUT_DEFAULT_TO_FOCUS_MS  20000
#define BEHAVIOR_TIMEOUT_FOCUS_TO_DEFAULT_MS  20000
#define BEHAVIOR_FOCUS_SWITCH_MIN_MS          5000
#define BEHAVIOR_FOCUS_SWITCH_MAX_MS          8000
#define BEHAVIOR_HAPPY_SWITCH_MIN_MS          8000
#define BEHAVIOR_HAPPY_SWITCH_MAX_MS          10000
#define BEHAVIOR_HAPPY_NO_FACE_TIMEOUT_MS     30000

void dooi_behavior_init(void);
void dooi_behavior_update(void);
void dooi_behavior_set_face_detected(bool has_face);
void dooi_behavior_enable(bool enable);
bool dooi_behavior_is_enabled(void);
dooi_behavior_state_t dooi_behavior_get_state(void);

/**
 * @brief 统一行为入口（逻辑层网关）
 *
 * 所有"让机器人执行某个行为"的请求（来自 web / mcp / xbox / 蓝牙 / 远程协议）
 * 都应通过此函数，而非直接调用 app_behavior_play_scene。
 * 逻辑层在此做集中式状态记录与（未来的）互斥/优先级裁决，再委托执行层播放。
 *
 * @param scene_name 场景名（与 app_behavior 场景表一致）
 * @param variant    变体名（可为 NULL）
 * @param prio       优先级
 * @return true 已提交播放
 */
bool app_ui_logic_request_behavior(const char *scene_name, const char *variant, int prio);

/**
 * @brief 读取最近一次被请求播放的场景名（逻辑层内部可见，用于状态机/调试）
 */
const char *app_ui_logic_last_behavior_scene(void);

#ifdef __cplusplus
}
#endif

#endif