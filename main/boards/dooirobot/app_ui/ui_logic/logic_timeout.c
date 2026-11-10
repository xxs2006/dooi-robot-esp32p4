#include "logic_timeout.h"

/* 超时状态机核心：纯状态对象，逻辑层唯一持有方（无 FreeRTOS/ESP 依赖） */
static logic_timeout_state_t s_timeout = {
    .current_screen = SCREEN_NULL,
    .old_screen     = SCREEN_NULL,
    .idle_timeout_ticks = 0,
};

void logic_timeout_set_screen(screen_type_t screen)          { s_timeout.current_screen = screen; }
void logic_timeout_set_old_screen(screen_type_t screen)      { s_timeout.old_screen     = screen; }
void logic_timeout_reset_idle_tick(void)                      { s_timeout.idle_timeout_ticks = 0; }

screen_type_t logic_timeout_get_screen(void)     { return s_timeout.current_screen; }
screen_type_t logic_timeout_get_old_screen(void) { return s_timeout.old_screen; }

void logic_timeout_tick_advance(int is_activation)
{
    if (s_timeout.current_screen == SCREEN_EYES) {
        s_timeout.idle_timeout_ticks++;
    }
}

/* ── 纯判定：无副作用 ── */
bool eyes_should_idle(void)
{
    if (s_timeout.current_screen != SCREEN_EYES) {
        return false;
    }
    if (s_timeout.idle_timeout_ticks <= APP_UI_MS_TO_TICKS(IDLE_TIMEOUT_MS)) {
        return false;
    }

    s_timeout.idle_timeout_ticks = 0;
    
    return true;
}
