#ifndef __LOGIC_VISION_H__
#define __LOGIC_VISION_H__

#include "app_ui_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 视觉任务启停 */
void on_vision_task_exited(const app_event_t *evt);
void on_vision_task_started(const app_event_t *evt);

/* 人脸识别 */
void on_face_detected(const app_event_t *evt);
void on_face_aligned(const app_event_t *evt);

/* 手势识别 */
void on_gesture(const app_event_t *evt);

#ifdef __cplusplus
}
#endif

#endif /* __LOGIC_VISION_H__ */
