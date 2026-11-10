#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "app_vision.h"   // vision_result_t

#ifdef __cplusplus
extern "C" {
#endif

//====================================================
// 追踪配置
//====================================================
#define FACE_TRACK_CENTER_X            100
#define FACE_TRACK_CENTER_Y            100
#define FACE_TRACK_DEAD_ZONE           25
#define FACE_TRACK_MAX_ANGLE           30.0f
#define FACE_TRACK_MIN_ANGLE           3.0f
#define FACE_TRACK_PIXEL_TO_ANGLE      0.25f
#define FACE_TRACK_TURN_SPEED          35
#define FACE_TRACK_CORRECT_TIMEOUT_MS  4000
#define FACE_TRACK_LOST_TIMEOUT_MS     2000
#define FACE_TRACK_FACE_CACHE_VALID_MS 500
#define FACE_TRACK_TRACK_INTERVAL_MS   900

//====================================================
// 状态枚举
//====================================================
typedef enum {
    FACE_TRACK_STATE_IDLE = 0,
    FACE_TRACK_STATE_CORRECTING,
    FACE_TRACK_STATE_TRACKING,
    FACE_TRACK_STATE_LOST,
} face_track_state_t;

//====================================================
// 回调类型
//
//  get_face_result_cb  — 让调用方把当前检测结果填入 out_result，
//                        返回 true 表示本帧有有效人脸
//  map_to_display_cb   — 模型坐标 → 显示坐标映射
//  on_status_text_cb   — 追踪器需要更新左屏文字时回调
//  on_face_aligned_cb  — 单次对齐完成时回调（continuous=false 对齐后触发）
//====================================================
typedef bool (*face_track_get_result_cb_t)(vision_result_t *out_result, void *user_data);

typedef void (*face_track_map_display_cb_t)(int mx,  int my,  int mw,  int mh,
                                            int *dx, int *dy, int *dw, int *dh,
                                            void *user_data);

typedef void (*face_track_status_cb_t)(const char *text, void *user_data);

typedef void (*face_track_aligned_cb_t)(void *user_data);

//====================================================
// 配置结构（init 时传入）
//====================================================
typedef struct {
    face_track_get_result_cb_t get_result;    // 必须提供
    face_track_map_display_cb_t map_display;  // 必须提供
    face_track_status_cb_t     on_status;     // 必须提供
    face_track_aligned_cb_t    on_aligned;    // 可为 NULL
    void                      *user_data;     // 透传给所有回调
} face_tracker_config_t;

//====================================================
// 公开 API（单例模式）
//====================================================

/**
 * @brief 初始化追踪器，注入外部依赖（必须在 enable 之前调用）
 * @param cfg 配置结构；传 NULL 表示清空回调（页面卸载时使用），此时不会自动启用追踪
 */
void face_tracker_init(const face_tracker_config_t *cfg);

/**
 * @brief 解绑所有回调（页面卸载时必须调用）
 *        只清除回调、不停用追踪：清空后数据源自动切到 face_tracker_push_result()。
 *        不清除的话，on_status 会持有已被销毁的 LVGL 对象指针而崩溃。
 */
void face_tracker_unbind(void);

/**
 * @brief 反初始化：停用追踪、停止电机并解绑所有回调。
 */
void face_tracker_deinit(void);

/**
 * @brief 启用 / 停用追踪
 * @param enable     true=启用，false=停用
 * @param continuous true=持续追踪，false=对齐一次后停止
 */
void face_tracker_enable(bool enable, bool continuous);

/**
 * @brief 每帧调用一次，驱动状态机
 */
void face_tracker_update(void);

/**
 * @brief 获取当前状态（只读）
 */
face_track_state_t face_tracker_get_state(void);

/**
 * @brief 获取启用状态（只读）
 */
bool face_tracker_is_enabled(void);

/**
 * @brief 推送人脸检测结果给追踪器（跨页面追踪用）
 *        视觉模块检测到人脸后调用此接口，追踪器会使用这些结果进行追踪
 * @param result 人脸检测结果
 * @return true 表示结果已接受，false 表示追踪器未初始化或未启用
 */
bool face_tracker_push_result(const vision_result_t *result);

#ifdef __cplusplus
}
#endif