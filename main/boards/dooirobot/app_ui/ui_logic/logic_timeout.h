#pragma once
#include <stdint.h>
#include <stdbool.h>

#include "app_ui.h"  

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    screen_type_t current_screen;
    screen_type_t old_screen;
    uint16_t idle_timeout_ticks;
} logic_timeout_state_t;

/* 状态写入 */
void logic_timeout_set_screen(screen_type_t screen);
void logic_timeout_set_old_screen(screen_type_t screen);
void logic_timeout_reset_idle_tick(void);

/* 状态读取 */
screen_type_t logic_timeout_get_screen(void);
screen_type_t logic_timeout_get_old_screen(void);

/* 心跳推进计数（与 on_timer_tick 语义一致：先推进再判定） */
void logic_timeout_tick_advance(int is_activation);

/* ── 纯判定函数：无副作用，供 PC 单测直接调用 ── */
bool eyes_should_idle(void);

#ifdef __cplusplus
}
#endif
