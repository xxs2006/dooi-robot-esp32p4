#ifndef __LOGIC_PERIPHERAL_H__
#define __LOGIC_PERIPHERAL_H__

#include "app_ui_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 外设 / 硬件相关事件处理模块
 *
 * 收集相机预览、红外避障、触摸按键、IMU 姿态等硬件事件的业务逻辑，
 * 与系统状态、视觉识别逻辑解耦。
 */

/* 相机预览触发 */
void on_camera_trigger_preview(const app_event_t *evt);

/* 红外避障 */
void on_ir_enable(const app_event_t *evt);
void on_ir_disable(const app_event_t *evt);
void on_ir_trigger(const app_event_t *evt);

/* IMU 姿态 / 倾斜 / 推动 / 抱起 */
void on_imu(const app_event_t *evt);

/* 触摸按键：双长按检测 + 按当前屏幕分发到 logic_page_*_on_key */
void on_button(const app_event_t *evt);

#ifdef __cplusplus
}
#endif

#endif /* __LOGIC_PERIPHERAL_H__ */
