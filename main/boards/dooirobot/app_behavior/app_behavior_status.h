#ifndef APP_BEHAVIOR_STATUS_H_
#define APP_BEHAVIOR_STATUS_H_

#include "app_behavior_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 行为状态枚举。
 *
 * 每个枚举值对应 resources/status/<name>.json 中的一个状态资源文件。
 * 文件名由 app_behavior_status_name() 从映射表查得，调用方无需再手写字符串，
 * 从而在编译期获得类型安全，避免拼写错误。
 */
typedef enum {
    BEHAVIOR_STATUS_IDLE = 0,        /* status_idle.json         */
    BEHAVIOR_STATUS_STANDBY,         /* status_standby.json       */
    BEHAVIOR_STATUS_PAGE_MUSIC,      /* status_on_music.json      */

    BEHAVIOR_STATUS_COUNT,           /* 枚举总数（哨兵，非真实状态） */
} behavior_status_id_t;

/**
 * @brief 由状态枚举查得对应的资源文件名（不含 .json 后缀）。
 * @param id 状态枚举值
 * @return 资源文件名字符串；非法 id 返回 "status_idle"
 */
const char *app_behavior_status_name(behavior_status_id_t id);

/**
 * @brief 由资源文件名（不含 .json 后缀）反查状态枚举，用于解析外部/配置输入。
 * @param name 资源文件名
 * @param out_id 输出枚举；成功时写入
 * @return true 找到匹配枚举，false 未找到（out_id 不修改）
 */
bool app_behavior_status_from_name(const char *name, behavior_status_id_t *out_id);

/**
 * @brief 初始化行为状态管理，并注册“场景结束后恢复”钩子到 app_behavior。
 *        应在 app_behavior_init() 之后调用。
 */
void app_behavior_status_init(void);

/**
 * @brief 设置（持久）状态并播放对应 status JSON。
 *        该状态会成为之后场景结束时的“恢复目标”。
 * @param id 状态枚举
 */
void app_behavior_status_set(behavior_status_id_t id);

/**
 * @brief 进入嵌套状态：旧状态压入栈，id 成为新的“恢复目标”并播放。
 *        典型用法：进入对话状态 push(BEHAVIOR_STATUS_PAGE_SELECT)，退出对话 pop() 回到 idle。
 */
void app_behavior_status_push(behavior_status_id_t id);

/** @brief 退出嵌套状态：恢复栈顶状态并播放 */
void app_behavior_status_pop(void);

/** @brief 当前恢复目标状态枚举（未初始化返回 BEHAVIOR_STATUS_IDLE） */
behavior_status_id_t app_behavior_status_current(void);

/* 当前激活状态的资源名（无 .json 后缀），用于 preload 比对 */
const char *app_behavior_status_current_name(void);

/* 按资源名（无 .json）设置当前状态；内部已对相同状态去重，未知名忽略 */
void app_behavior_status_set_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* APP_BEHAVIOR_STATUS_H_ */
