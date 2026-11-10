// eyes_control.h
#ifndef EYES_CONTROL_H
#define EYES_CONTROL_H

#include "eyes_core.h"

// 定义自动切换的时间间隔范围（单位：毫秒）
#define MIN_SWITCH_INTERVAL_MS 8000
#define MAX_SWITCH_INTERVAL_MS 15000

// 定义随机表情的范围
#define RANDOM_EMOTION_START EYES_EMOTION_FOCUS_BELOW_CENTER
#define RANDOM_EMOTION_END   EYES_EMOTION_AMAZED

void eye_controller_apply_emotion(EYES_EMOTION emotion);
void eye_controller_set_emotion_auto(EYES_EMOTION emotion);
bool eye_controller_is_manual_emotion(void);

/* ---- 运行时直调（供网页 /api/eyes 与外部工具 HTTP 使用） ----
 * 库已提供：eye_controller_apply_preset() / eye_controller_set_lid()。
 * 这里的包装负责： preset/单眼直调 前同步"手动保持"状态（推迟自动随机切换），
 * 并对会改渲染状态的调用加 eyes 临界保护（与库内 set_emotion 的做法一致）。 */
void eyes_control_apply_preset(const EyeExpressionPreset *preset);
void eyes_control_set_lid(uint32_t eye_index, EYES_LIDS_DEFINE lid_type,
                          float upper, float lower, uint32_t duration_ms);
void eyes_control_gaze(uint32_t eye_index, float x, float y, int32_t duration_ms);
void eyes_control_pupil(uint32_t eye_index, float ratio, int32_t duration_ms);
void eyes_control_scale(uint32_t eye_index, float ratio, int32_t duration_ms);
void eyes_control_blink(uint32_t duration);
void eyes_control_wink(uint32_t eye_index);

int eyes_render_init(void);
void eyes_render_task(void *arg);

#endif