#ifndef __LOGIC_SYS_STATUS_H__
#define __LOGIC_SYS_STATUS_H__

#include <stdint.h>
#include "app_ui_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 系统 / 对话状态、连接与电源相关事件处理模块
 *
 * 该模块把原本集中在 app_ui_logic.c 中的系统状态机、蓝牙/WebServer 连接、
 * 电源按钮（多击配网/复位）以及电池状态等事件处理拆分到独立文件，
 * 由 app_ui_logic 的事件分发表统一调用。
 */

/* 系统对话状态事件 */
void on_state_loading_protocol(const app_event_t *evt);
void on_state_connect_wifi(const app_event_t *evt);
void on_state_standby(const app_event_t *evt);
void on_state_listening(const app_event_t *evt);
void on_state_speaking(const app_event_t *evt);
void on_state_connecting(const app_event_t *evt);
void on_state_activation(const app_event_t *evt);
void on_state_checking_new_version(const app_event_t *evt);
void on_state_error(const app_event_t *evt);
void on_state_idle(const app_event_t *evt);

/* 蓝牙 / WebServer 连接事件 */
void on_bluetooth_open(const app_event_t *evt);
void on_bluetooth_close(const app_event_t *evt);
void on_web_server_connect(const app_event_t *evt);
void on_web_server_disconnect(const app_event_t *evt);

/* 电源按钮与电池状态事件 */
void on_power_btn_press(const app_event_t *evt);
void on_power_shutdown(const app_event_t *evt);
void on_power_status(const app_event_t *evt);

#ifdef __cplusplus
}
#endif

#endif /* __LOGIC_SYS_STATUS_H__ */
