#include "app_ui_logic.h"

#include "app_ui.h"
#include "app_vision.h"
#include "face_tracker.h"

#include "logic_sys_status.h"
#include "logic_peripheral.h"
#include "logic_vision.h"
#include "dooi_behavior.h"
#include "app_behavior_status.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "logic_timeout.h"

static const char *TAG = "ui_logic";

static QueueHandle_t s_evt_queue = NULL;

app_system_state_t g_app_state = {
    .current_state = APP_EVT_STATE_LOADING_PROTOCOL,
    .prev_state    = APP_EVT_STATE_LOADING_PROTOCOL,
};

typedef void (*app_event_handler_t)(const app_event_t *evt);

void set_conv_state(app_event_type_t new_state)
{
    if (new_state == g_app_state.current_state) return;
    g_app_state.prev_state    = g_app_state.current_state;
    g_app_state.current_state = new_state;
}

static const app_event_handler_t s_handler_table[APP_EVT_COUNT] = {
    [APP_EVT_STATE_LOADING_PROTOCOL]     = on_state_loading_protocol,
    [APP_EVT_STATE_CONNECT_WIFI]         = on_state_connect_wifi,
    [APP_EVT_STATE_STANDBY]              = on_state_standby,
    [APP_EVT_STATE_LISTENING]            = on_state_listening,
    [APP_EVT_STATE_SPEAKING]             = on_state_speaking,
    [APP_EVT_STATE_CONNECTING]           = on_state_connecting,
    [APP_EVT_STATE_ACTIVATION]           = on_state_activation,
    [APP_EVT_STATE_CHECKING_NEW_VERSION] = on_state_checking_new_version,
    [APP_EVT_STATE_ERROR]                = on_state_error,
    [APP_EVT_STATE_IDLE]                 = on_state_idle,

    [APP_EVT_BLUETOOTH_OPEN]             = on_bluetooth_open,
    [APP_EVT_BLUETOOTH_CLOSE]            = on_bluetooth_close,
    [APP_EVT_WEB_SERVER_CONNECT]         = on_web_server_connect,
    [APP_EVT_WEB_SERVER_DISCONNECT]      = on_web_server_disconnect,

    [APP_EVT_VISION_TASK_EXITED]         = on_vision_task_exited,
    [APP_EVT_VISION_TASK_STARTED]        = on_vision_task_started,
    [APP_EVT_CAMERA_TIGGRER_PREVIEW]     = on_camera_trigger_preview,
    [APP_EVT_IR_ENABLE]                  = on_ir_enable,
    [APP_EVT_IR_DISABLE]                 = on_ir_disable,
    [APP_EVT_IR_TRIGGER]                 = on_ir_trigger,
    [APP_EVT_BUTTON]                     = on_button,
    [APP_EVT_POWER_BTN_PRESS]            = on_power_btn_press,
    [APP_EVT_POWER_SHUTDOWN]             = on_power_shutdown,
    [APP_EVT_IMU]                        = on_imu,
    [APP_EVT_POWER_STATUS]               = on_power_status,

    [APP_EVT_FACE_DETECTED]              = on_face_detected,
    [APP_EVT_FACE_ALIGNED]               = on_face_aligned,            // 人脸对齐
    [APP_EVT_GESTURE]                    = on_gesture,
};

/* -----------------------------------------------------------------------
 * 逻辑任务
 * --------------------------------------------------------------------- */
static void logic_task(void *arg)
{
    app_event_t evt;
    while (1) {
        if (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY) == pdTRUE) {
            if (evt.type < APP_EVT_COUNT && s_handler_table[evt.type]) {
                s_handler_table[evt.type](&evt);
            } else {
                ESP_LOGW(TAG, "Unhandled event type: %d", evt.type);
            }
        }
    }
}

bool app_ui_logic_post(const app_event_t *evt, bool from_isr)
{
    if (!s_evt_queue) return false;
    if (from_isr) {
        BaseType_t woken = pdFALSE;
        bool ok = xQueueSendFromISR(s_evt_queue, evt, &woken) == pdTRUE;
        portYIELD_FROM_ISR(woken);
        return ok;
    }
    return xQueueSend(s_evt_queue, evt, pdMS_TO_TICKS(10)) == pdTRUE;
}

bool app_ui_logic_is_init_done(void)
{
    return g_app_state.sys_is_init_done;
}

app_event_type_t app_ui_logic_get_state(void)
{
    return g_app_state.current_state;
}

/* -----------------------------------------------------------------------
 * 辅助：判断某状态是否属于"对话中"（LISTENING 或 SPEAKING）
 * --------------------------------------------------------------------- */
bool app_ui_logic_is_conv_state()
{
    return g_app_state.is_conv_state;
}

// 视觉线程的回调：把底层视觉结果转换为事件投递到逻辑队列
static void on_vision_result(vision_mode_t model_type,
                             const vision_result_t *results,
                             size_t count,
                             void *user_data)
{
    if (app_ui_logic_get_current_screen() == SCREEN_CAMERA) {
        camera_on_vision_result_to_screen(model_type, results, count, user_data);
    }

    if (model_type == VISION_MODE_FACE_DETECT && count > 0) {
        // ESP_LOGI(TAG, "Face detected with score: %.2f", results[0].score);

        app_event_t evt = {.type = APP_EVT_FACE_DETECTED, .data.vision_result = {.result = results[0]}};
        app_ui_logic_post(&evt, false);
    }

    if (model_type == VISION_MODE_GESTURE_RECOGNIZE && count > 0) {
        gesture_type_t current = gesture_type_from_label(results[0].label);

        // ESP_LOGI(TAG, "Gesture recognized: %s", gesture_type_to_str(current));

        static gesture_type_t s_last_gesture = GESTURE_UNKNOWN;

        bool is_valid = (current != GESTURE_NO_HAND &&
                         current != GESTURE_NO_GESTURE &&
                         current != GESTURE_UNKNOWN);

        if (is_valid && current != s_last_gesture) {
            app_event_t evt = {
                .type = APP_EVT_GESTURE,
                .data.gesture_data = {
                    .current  = current,
                    .previous = s_last_gesture,
                    .score    = results[0].score,
                },
            };
            app_ui_logic_post(&evt, false);

            s_last_gesture = current;
        }
    }
}

void app_ui_logic_request_screen(screen_type_t screen)
{
    /* 切屏事件驱动页面状态：进入音乐页面进入 on music 状态，离开音乐页面回到待机 */
    screen_type_t old_screen = logic_timeout_get_screen();
    if (screen == SCREEN_MUSIC_SPECTRUM) {
        app_behavior_status_set(BEHAVIOR_STATUS_PAGE_MUSIC);
    } else if (old_screen == SCREEN_MUSIC_SPECTRUM) {
        app_behavior_status_set(BEHAVIOR_STATUS_STANDBY);
    }

    logic_timeout_set_screen(screen);

    /* screen_set_current 仅设置 next_screen 标志，由 UI 线程在 screen_update 中消费执行 */
    screen_set_current(screen);
}

void app_ui_logic_request_idle(int trigger)
{
    if (trigger) {
        screen_idle_timeout_trigger();
    } else {
        screen_idle_timeout_reset();
    }
}

bool app_ui_logic_request_shutdown(void)
{
    app_event_t evt = { .type = APP_EVT_POWER_SHUTDOWN };
    bool ok = app_ui_logic_post(&evt, false);
    if (!ok) {
        ESP_LOGW(TAG, "Shutdown request dropped: logic layer not ready or queue busy");
    }
    return ok;
}

void app_ui_logic_report_screen(screen_type_t current, screen_type_t old)
{
    logic_timeout_set_screen(current);
    logic_timeout_set_old_screen(old);
}

screen_type_t app_ui_logic_get_current_screen(void)
{
    return logic_timeout_get_screen();
}

void app_ui_logic_tick(void)
{
    int is_activation = (app_ui_logic_get_state() == APP_EVT_STATE_ACTIVATION);

    /* 计数推进（先推进，再判定，与原语义一致） */
    logic_timeout_tick_advance(is_activation);

    /* 待机超时 → 切换到时钟页面并启动人脸检测 */
    if (eyes_should_idle()) {
        app_event_t idle_evt = {.type = APP_EVT_STATE_IDLE};
        app_ui_logic_post(&idle_evt, false);
    }
}

void app_ui_logic_init(void)
{
    s_evt_queue = xQueueCreate(LOGIC_QUEUE_DEPTH, sizeof(app_event_t));
    configASSERT(s_evt_queue);

    app_vision_set_callback(on_vision_result, NULL);
    face_tracker_init(NULL);

    /* 开机即启用跨页面人脸追踪。
       此时 cfg 为空，priv_get_offset() 会自动走 external_face 分支，
       数据源是 on_face_detected() → face_tracker_push_result()，
       因此在任意页面（眼睛/时钟等）检测到人脸都会自动转向。
       注意：必须显式调用 enable，face_tracker_init(NULL) 不会启用。 */
    face_tracker_enable(true, true);

    dooi_behavior_init();

    xTaskCreatePinnedToCore( logic_task, "ui_logic", LOGIC_TASK_STACK, NULL, LOGIC_TASK_PRIORITY, NULL, 0);
    ESP_LOGI(TAG, "app_ui_logic initialized");
}