#include "dooi_behavior.h"
#include "eyes_core.h"
#include "eyes_config.h"
#include "eyes_control.h"
#include "app_ui_logic.h"
#include "app_behavior.h"
#include "esp_log.h"

static const char *TAG = "dooi_behavior";

static bool s_enabled = false;
static dooi_behavior_state_t s_state = BEHAVIOR_STATE_IDLE;

static uint32_t s_state_start_time = 0;
static uint32_t s_emotion_change_time = 0;
static uint32_t s_next_emotion_interval = 0;
static uint32_t s_last_face_seen_time = 0;

#define FACE_DETECTION_VALID_MS 2000

static bool behavior_has_face(void)
{
    uint32_t now = eyes_get_tick_ms();
    return (now - s_last_face_seen_time) < FACE_DETECTION_VALID_MS;
}

static const char *behavior_state_to_str(dooi_behavior_state_t state)
{
    switch (state) {
        case BEHAVIOR_STATE_IDLE:    return "IDLE";
        case BEHAVIOR_STATE_DEFAULT: return "DEFAULT";
        case BEHAVIOR_STATE_FOCUS:   return "FOCUS";
        case BEHAVIOR_STATE_HAPPY:   return "HAPPY";
        default:                     return "UNKNOWN";
    }
}

static void behavior_apply_emotion(EYES_EMOTION emotion)
{
    eye_controller_set_emotion_auto(emotion);
    s_emotion_change_time = eyes_get_tick_ms();
}

static void behavior_transition_to(dooi_behavior_state_t new_state, EYES_EMOTION emotion)
{
    // ESP_LOGI(TAG, "State transition: %s -> %s", behavior_state_to_str(s_state), behavior_state_to_str(new_state));
    s_state = new_state;
    s_state_start_time = eyes_get_tick_ms();
    eye_controller_set_emotion_auto(emotion);
    s_emotion_change_time = s_state_start_time;
}

static EYES_EMOTION behavior_get_random_focus_emotion(void)
{
    EYES_EMOTION emotion = (EYES_EMOTION)(EYES_EMOTION_FOCUS_BELOW_CENTER + (eyes_rand32() % 6));
    return emotion;
}

static EYES_EMOTION behavior_get_random_happy_emotion(void)
{
    EYES_EMOTION emotion = (EYES_EMOTION)(EYES_EMOTION_HAPPY + (eyes_rand32() % 3));
    return emotion;
}

static void behavior_update_default(void)
{
    uint32_t now = eyes_get_tick_ms();

    if (behavior_has_face()) {
        s_next_emotion_interval = urandom(BEHAVIOR_HAPPY_SWITCH_MIN_MS, BEHAVIOR_HAPPY_SWITCH_MAX_MS);
        behavior_transition_to(BEHAVIOR_STATE_HAPPY, behavior_get_random_happy_emotion());
        return;
    }

    if ((now - s_state_start_time) >= BEHAVIOR_TIMEOUT_DEFAULT_TO_FOCUS_MS) {
        s_next_emotion_interval = urandom(BEHAVIOR_FOCUS_SWITCH_MIN_MS, BEHAVIOR_FOCUS_SWITCH_MAX_MS);
        behavior_transition_to(BEHAVIOR_STATE_FOCUS, behavior_get_random_focus_emotion());
        return;
    }
}

static void behavior_update_focus(void)
{
    uint32_t now = eyes_get_tick_ms();

    if (behavior_has_face()) {
        s_next_emotion_interval = urandom(BEHAVIOR_HAPPY_SWITCH_MIN_MS, BEHAVIOR_HAPPY_SWITCH_MAX_MS);
        behavior_transition_to(BEHAVIOR_STATE_HAPPY, behavior_get_random_happy_emotion());
        return;
    }

    if ((now - s_state_start_time) >= BEHAVIOR_TIMEOUT_FOCUS_TO_DEFAULT_MS) {
        behavior_transition_to(BEHAVIOR_STATE_DEFAULT, EYES_EMOTION_DEFAULT);
        return;
    }

    if ((now - s_emotion_change_time) >= s_next_emotion_interval) {
        s_next_emotion_interval = urandom(BEHAVIOR_FOCUS_SWITCH_MIN_MS, BEHAVIOR_FOCUS_SWITCH_MAX_MS);
        behavior_apply_emotion(behavior_get_random_focus_emotion());
        return;
    }
}

static void behavior_update_happy(void)
{
    uint32_t now = eyes_get_tick_ms();
    uint32_t no_face_duration = now - s_last_face_seen_time;

    if (no_face_duration >= BEHAVIOR_HAPPY_NO_FACE_TIMEOUT_MS) {
        behavior_transition_to(BEHAVIOR_STATE_DEFAULT, EYES_EMOTION_DEFAULT);
        return;
    }

    if ((now - s_emotion_change_time) >= s_next_emotion_interval) {
        s_next_emotion_interval = urandom(BEHAVIOR_HAPPY_SWITCH_MIN_MS, BEHAVIOR_HAPPY_SWITCH_MAX_MS);
        behavior_apply_emotion(behavior_get_random_happy_emotion());
        return;
    }
}

void dooi_behavior_init(void)
{
    s_enabled = false;
    s_state = BEHAVIOR_STATE_IDLE;
    s_state_start_time = 0;
    s_emotion_change_time = 0;
    s_next_emotion_interval = 0;
    s_last_face_seen_time = 0;
}

void dooi_behavior_update(void)
{
    if (!s_enabled) return;
    if (app_ui_logic_is_conv_state() || eye_controller_is_manual_emotion()) return;

    switch (s_state) {
        case BEHAVIOR_STATE_DEFAULT:
            behavior_update_default();
            break;

        case BEHAVIOR_STATE_FOCUS:
            behavior_update_focus();
            break;

        case BEHAVIOR_STATE_HAPPY:
            behavior_update_happy();
            break;

        case BEHAVIOR_STATE_IDLE:
        default:
            break;
    }
}

void dooi_behavior_set_face_detected(bool has_face)
{
    if (has_face) {
        s_last_face_seen_time = eyes_get_tick_ms();
    }
}

void dooi_behavior_enable(bool enable)
{
    if (s_enabled == enable) return;

    s_enabled = enable;

    if (enable) {
        behavior_transition_to(BEHAVIOR_STATE_DEFAULT, EYES_EMOTION_DEFAULT);
        s_last_face_seen_time = 0;
    } else {
        s_state = BEHAVIOR_STATE_IDLE;
    }
}

bool dooi_behavior_is_enabled(void)
{
    return s_enabled;
}

dooi_behavior_state_t dooi_behavior_get_state(void)
{
    return s_state;
}

static char s_last_scene[32] = {0};

bool app_ui_logic_request_behavior(const char *scene_name, const char *variant, int prio)
{
    if (!scene_name) return false;

    strncpy(s_last_scene, scene_name, sizeof(s_last_scene) - 1);
    s_last_scene[sizeof(s_last_scene) - 1] = '\0';

    if (strncmp(scene_name, "status_", 7) == 0) {
        return app_behavior_play_status(scene_name);
    }
    return app_behavior_play_scene(scene_name, variant, prio);
}

const char *app_ui_logic_last_behavior_scene(void)
{
    return s_last_scene;
}