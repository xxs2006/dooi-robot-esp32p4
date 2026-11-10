#include "logic_vision.h"

#include "app_vision.h"
#include "app_ui_logic.h"
#include "dooi_behavior.h"
#include "app_behavior.h"
#include "face_tracker.h"
#include "app_behavior_status.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "logic_vision";

/* -----------------------------------------------------------------------
 * 视觉任务启停
 * --------------------------------------------------------------------- */
void on_vision_task_exited(const app_event_t *evt)
{
    app_vision_priority_boost(10);  // 提升优先级，确保视觉任务 先执行
    app_vision_stop();
}

void on_vision_task_started(const app_event_t *evt)
{
    static bool first_start = true;
    vision_mode_t mode = evt->data.vision_task_data.mode;
    vision_intervals_t intervals = evt->data.vision_task_data.intervals;

    if(first_start)
    {
        vTaskDelay(pdMS_TO_TICKS(2000));
        first_start = false;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    app_vision_priority_restore();
    app_vision_start(mode, &intervals);
}

/* -----------------------------------------------------------------------
 * 人脸识别
 * --------------------------------------------------------------------- */
void on_face_detected(const app_event_t *evt)
{
    dooi_behavior_set_face_detected(true);

    face_tracker_push_result(&evt->data.vision_result.result);

    app_behavior_status_set(BEHAVIOR_STATUS_STANDBY);

    ESP_LOGI(TAG, "Face Detected: %.2f", evt->data.vision_result.result.score);
}

void on_face_aligned(const app_event_t *evt)
{
    ESP_LOGI(TAG, "Face Aligned!");
}

/* -----------------------------------------------------------------------
 * 手势识别
 * --------------------------------------------------------------------- */
void on_gesture(const app_event_t *evt)
{
    gesture_type_t gesture  = evt->data.gesture_data.current;
    gesture_type_t previous = evt->data.gesture_data.previous;
    float score             = evt->data.gesture_data.score;

    if(gesture == previous || score < 0.95){
        return;
    }

    ESP_LOGI(TAG, "Gesture event: %s (score=%.2f, prev=%s)",
             gesture_type_to_str(gesture), score,
             gesture_type_to_str(previous));

    switch (gesture) {
    case GESTURE_ONE:
        app_behavior_play_scene("action_greeting", NULL, BEHAVIOR_PRIO_DEFAULT);
        break;

    case GESTURE_TWO:
        app_behavior_play_scene("action_nod_agree", NULL, BEHAVIOR_PRIO_DEFAULT);
        break;

    case GESTURE_THREE:
        app_behavior_play_scene("action_dance", NULL, BEHAVIOR_PRIO_DEFAULT);
        break;

    case GESTURE_FOUR:
        app_behavior_play_scene("action_hug", NULL, BEHAVIOR_PRIO_DEFAULT);
        break;

    case GESTURE_FIVE:
        app_behavior_play_scene("action_welcome", NULL, BEHAVIOR_PRIO_DEFAULT);
        break;

    case GESTURE_OK:
        app_behavior_play_scene("happy", "bounce_jump", BEHAVIOR_PRIO_DEFAULT);
        break;

    case GESTURE_LIKE:
        app_behavior_play_scene("happy", NULL, BEHAVIOR_PRIO_DEFAULT);
        break;

    case GESTURE_DISLIKE:
        app_behavior_play_scene("angry", NULL, BEHAVIOR_PRIO_DEFAULT);
        break;

    default:
        break;
    }
}
