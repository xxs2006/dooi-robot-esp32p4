#ifndef __APP_VISION_H__
#define __APP_VISION_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── 运行模式 ── */
typedef enum {
    VISION_MODE_NONE = 0,
    VISION_MODE_FACE_DETECT,
    VISION_MODE_GESTURE_RECOGNIZE,
    VISION_MODE_ROUND_ROBIN,
    VISION_MODE_MAX
} vision_mode_t;

/* ── 手势类型枚举 ── */
typedef enum {
    GESTURE_ONE = 0,
    GESTURE_TWO,
    GESTURE_THREE,
    GESTURE_FOUR,
    GESTURE_FIVE,
    GESTURE_LIKE,
    GESTURE_OK,
    GESTURE_NO_GESTURE,
    GESTURE_CALL,
    GESTURE_DISLIKE,
    GESTURE_NO_HAND,
    GESTURE_UNKNOWN,
    GESTURE_COUNT
} gesture_type_t;

/* ── 检测结果 ── */
typedef struct {
    const char *label;
    float       score;
    int         box[4];  // x1, y1, x2, y2（分类任务中无意义，填 0）
} vision_result_t;

/* ── 结果回调（注册一次，全局生效） ── */
typedef void (*vision_result_cb_t)(
    vision_mode_t          mode,
    const vision_result_t *results,
    size_t                 count,
    void                  *user_data
);

gesture_type_t gesture_type_from_label(const char *label);
const char *gesture_type_to_str(gesture_type_t type);

/**
 * @brief  注册结果回调（只需调用一次，stop 后仍保留）
 */
void app_vision_set_callback(vision_result_cb_t cb, void *user_data);

typedef struct {
    uint32_t face_detect_ms;
    uint32_t gesture_recognize_ms;
} vision_intervals_t;

#define VISION_INTERVALS_DEFAULT() { \
    .face_detect_ms = 2000, \
    .gesture_recognize_ms = 2000 \
}

/**
 * @brief  启动视觉推理（首次自动初始化 + 加载模型）
 * @param  mode         运行模式
 * @param  intervals    各模式间隔配置（NULL 使用默认值）
 */
bool app_vision_start(vision_mode_t mode, const vision_intervals_t *intervals);

/**
 * @brief  停止并释放所有资源
 */
void app_vision_stop(void);

/**
 * @brief  运行中切换模式
 */
bool app_vision_switch_mode(vision_mode_t mode);

/**
 * @brief  运行中修改推理间隔
 */
void app_vision_set_interval(uint32_t interval_ms);

void app_model_mutex_init(void);
void app_model_mutex_lock(void);
void app_model_mutex_unlock(void);

void app_vision_priority_boost(int task_priority);
void app_vision_priority_restore(void);


#ifdef __cplusplus
}
#endif

#endif /* __APP_VISION_H__ */