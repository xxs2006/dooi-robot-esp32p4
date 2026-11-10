// logic_emotion.c
// 眼睛情绪自动切换状态机（从 eyes_control.c 的 eye_controller_auto_update_emotion 迁入）
#include "logic_emotion.h"

#include "eyes_control.h"
#include "eyes_config.h"
#include "app_ui_logic.h"

/* ---- 状态机内部状态 ---- */
static bool      s_manual_emotion            = false;
static uint32_t  s_last_emotion_update_time  = 0;
static uint32_t  s_next_emotion_update_interval = 0;

void logic_emotion_set_manual(EYES_EMOTION emotion)
{
    if (emotion >= EYES_EMOTION_COUNT) {
        return;
    }
    eye_controller_set_emotion(emotion);

    s_last_emotion_update_time = eyes_get_tick_ms();

    uint32_t interval_range = MAX_SWITCH_INTERVAL_MS - MIN_SWITCH_INTERVAL_MS;
    s_next_emotion_update_interval =
        (eyes_rand32() % (interval_range + 1)) + MIN_SWITCH_INTERVAL_MS;

    s_manual_emotion = true;
}

void logic_emotion_set_auto(EYES_EMOTION emotion)
{
    if (emotion >= EYES_EMOTION_COUNT) {
        return;
    }
    eye_controller_set_emotion(emotion);

    s_last_emotion_update_time = eyes_get_tick_ms();

    uint32_t interval_range = MAX_SWITCH_INTERVAL_MS - MIN_SWITCH_INTERVAL_MS;
    s_next_emotion_update_interval =
        (eyes_rand32() % (interval_range + 1)) + MIN_SWITCH_INTERVAL_MS;

    s_manual_emotion = false;
}

bool logic_emotion_is_manual(void)
{
    return s_manual_emotion;
}

void logic_emotion_hold_custom(void)
{
    s_last_emotion_update_time = eyes_get_tick_ms();

    uint32_t interval_range = MAX_SWITCH_INTERVAL_MS - MIN_SWITCH_INTERVAL_MS;
    s_next_emotion_update_interval =
        (eyes_rand32() % (interval_range + 1)) + MIN_SWITCH_INTERVAL_MS;

    s_manual_emotion = true;
}

void logic_emotion_auto_update(void)
{
    uint32_t now = eyes_get_tick_ms();

    if (s_last_emotion_update_time == 0 ||
        (now - s_last_emotion_update_time >= s_next_emotion_update_interval))
    {
        EYES_EMOTION next;

        if (app_ui_logic_is_conv_state()) {
            static const EYES_EMOTION s_conv_emotions[] = {
                EYES_EMOTION_FOCUS_BELOW_CENTER,
                EYES_EMOTION_HAPPY1,
                EYES_EMOTION_SAD,
                EYES_EMOTION_SUSPICIOUS,
            };
            next = s_conv_emotions[eyes_rand32() %
                (sizeof(s_conv_emotions) / sizeof(s_conv_emotions[0]))];
            s_manual_emotion = false;
        } else if (s_manual_emotion) {
            int emotion_range = RANDOM_EMOTION_END - RANDOM_EMOTION_START + 1;
            next = (EYES_EMOTION)((eyes_rand32() % emotion_range) + RANDOM_EMOTION_START);
            s_manual_emotion = false;
        } else {
            return;
        }

        logic_emotion_set_auto(next);
    }
}
