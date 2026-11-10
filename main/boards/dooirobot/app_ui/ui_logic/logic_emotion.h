#pragma once

#include "eyes_control.h"   // EYES_EMOTION, EYES_EMOTION_COUNT

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * logic_emotion —— 眼睛"情绪自动切换"状态机（从 eyes_control.c 迁入）
 *
 * 原 eye_controller_auto_update_emotion 把"是否对话中 / 手动情绪 / 随机切换"
 * 的业务决策写进了渲染任务 (eyes_render_task)。该决策属于 UI 逻辑，现收口
 * 到 ui_logic：本模块持有计时与手动标记状态机，由 UI 心跳 (logic_page_eyes
 * 渲染节拍或 app_ui_logic 的 on_timer_tick) 驱动 logic_emotion_auto_update()。
 * 底层情绪设置仍经 eyes_control 的 eye_controller_set_emotion 落到渲染。
 * ========================================================================= */

/* 设置一次"手动情绪"（手动标记置位，自动切换临时让位） */
void logic_emotion_set_manual(EYES_EMOTION emotion);

/* 设置一次"自动产生"的情绪（自动标记置位） */
void logic_emotion_set_auto(EYES_EMOTION emotion);

/* 当前是否处于手动情绪状态 */
bool logic_emotion_is_manual(void);

/* 保持当前表情一段时间：不切换情绪，只推迟自动随机切换并置手动标记。
 * 供"运行时自定义表情/preset 预览"使用（网页调参后 ~8~15s 内不会被自动表情覆盖）。 */
void logic_emotion_hold_custom(void);

/* 每节拍推进：根据对话状态/手动状态决定下一次情绪并切换 */
void logic_emotion_auto_update(void);

#ifdef __cplusplus
}
#endif
