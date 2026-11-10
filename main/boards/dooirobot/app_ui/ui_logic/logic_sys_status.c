#include "logic_sys_status.h"

#include "logic_timeout.h"
#include "app_ui.h"
#include "app_ui_logic.h"
#include "app_ble_control.h"
#include "app_webserver.h"
#include "dooi_nvs.h"
#include "dooi_behavior.h"
#include "app_behavior_types.h"
#include "app_behavior_status.h"
#include "app_behavior.h"
#include "power_manager.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "logic_sys";

/* -----------------------------------------------------------------------
 * 系统 / 对话状态处理
 * --------------------------------------------------------------------- */
void on_state_loading_protocol(const app_event_t *evt)
{
    if (!g_app_state.sys_is_init_done) {
        g_app_state.sys_is_init_done = true;
        app_webserver_start();
        app_ui_logic_request_screen(SCREEN_EYES);
        app_event_t evt_to_post = {.type = APP_EVT_VISION_TASK_STARTED, .data.vision_task_data = {.mode = VISION_MODE_ROUND_ROBIN, .intervals = {.face_detect_ms = 10, .gesture_recognize_ms = 10}}};
        app_ui_logic_post(&evt_to_post, false);
    }
    set_conv_state(APP_EVT_STATE_LOADING_PROTOCOL);
}

void on_state_connect_wifi(const app_event_t *evt)
{
    set_conv_state(APP_EVT_STATE_CONNECT_WIFI);
}

void on_state_standby(const app_event_t *evt)
{
    app_event_t evt_to_post = {.type = APP_EVT_VISION_TASK_STARTED, .data.vision_task_data = {.mode = VISION_MODE_ROUND_ROBIN, .intervals = {.face_detect_ms = 10, .gesture_recognize_ms = 10}}};
    app_ui_logic_post(&evt_to_post, false);

    app_imu_enable_tilt_detect(true, 5.0f);

    if (app_ui_logic_is_conv_state()) {
        g_app_state.is_conv_state = false;
    }

    set_conv_state(APP_EVT_STATE_STANDBY);
}

void on_state_listening(const app_event_t *evt)
{
    // FIXME: 这里不太合理
    if(app_ui_logic_get_current_screen() != SCREEN_CAMERA)
    {
        app_event_t evt_to_post = {.type = APP_EVT_VISION_TASK_EXITED};
        app_ui_logic_post(&evt_to_post, false);
    }

    app_imu_enable_tilt_detect(false, 5.0f);

    app_ui_logic_request_idle(0);

    g_app_state.is_conv_state = true;
    set_conv_state(APP_EVT_STATE_LISTENING);
}

void on_state_speaking(const app_event_t *evt)
{
    set_conv_state(APP_EVT_STATE_SPEAKING);
}

void on_state_connecting(const app_event_t *evt)
{
    app_event_t evt_to_post = {.type = APP_EVT_VISION_TASK_EXITED};
    app_ui_logic_post(&evt_to_post, false);

    set_conv_state(APP_EVT_STATE_CONNECTING);
}

void on_state_activation(const app_event_t *evt)
{
    set_conv_state(APP_EVT_STATE_ACTIVATION);

    /* 语音唤醒触发：进入待机态（app_behavior_status_set 内部已防止重复进入） */
    app_behavior_status_set(BEHAVIOR_STATUS_STANDBY);
}

void on_state_checking_new_version(const app_event_t *evt)
{
    if (!g_app_state.sys_is_init_done) {
        g_app_state.sys_is_init_done = true;
        app_webserver_start();
        app_ui_logic_request_screen(SCREEN_EYES);
        app_event_t evt_to_post = {.type = APP_EVT_VISION_TASK_STARTED, .data.vision_task_data = {.mode = VISION_MODE_ROUND_ROBIN, .intervals = {.face_detect_ms = 10, .gesture_recognize_ms = 10}}};
        app_ui_logic_post(&evt_to_post, false);
    }
}

void on_state_error(const app_event_t *evt)
{
    ESP_LOGE(TAG, "Error state");
}

void on_state_idle(const app_event_t *evt)
{
    set_conv_state(APP_EVT_STATE_IDLE);

    app_event_t evt_to_post = {.type = APP_EVT_VISION_TASK_STARTED, .data.vision_task_data = {.mode = VISION_MODE_ROUND_ROBIN, .intervals = {.face_detect_ms = 10, .gesture_recognize_ms = 10}}};
    app_ui_logic_post(&evt_to_post, false);

    logic_timeout_reset_idle_tick();

    if (logic_timeout_get_screen() != SCREEN_DIGITAL_CLOCK) {
        digital_clock_mode_set(MODE_TIME);
        app_ui_logic_request_screen(SCREEN_DIGITAL_CLOCK);
    }
}

/* -----------------------------------------------------------------------
 * 蓝牙 / WebServer 连接事件
 * --------------------------------------------------------------------- */
void on_bluetooth_open(const app_event_t *evt)
{
    app_event_t evt_to_post = {.type = APP_EVT_VISION_TASK_EXITED};
    app_ui_logic_post(&evt_to_post, false);
}

void on_bluetooth_close(const app_event_t *evt)
{
    app_event_t evt_to_post = {.type = APP_EVT_VISION_TASK_STARTED, .data.vision_task_data = {.mode = VISION_MODE_ROUND_ROBIN, .intervals = {.face_detect_ms = 10, .gesture_recognize_ms = 10}}};
    app_ui_logic_post(&evt_to_post, false);
}

void on_web_server_connect(const app_event_t *evt)
{
    app_event_t evt_to_post = {.type = APP_EVT_VISION_TASK_EXITED};
    app_ui_logic_post(&evt_to_post, false);

    evt_to_post.type = APP_EVT_IR_ENABLE;
    app_ui_logic_post(&evt_to_post, false);
}

void on_web_server_disconnect(const app_event_t *evt)
{
    app_event_t evt_to_post = {.type = APP_EVT_IR_DISABLE};
    app_ui_logic_post(&evt_to_post, false);
}

/* -----------------------------------------------------------------------
 * 电源按钮 / 电池状态
 * --------------------------------------------------------------------- */
extern void dooi_enter_wifi_config_mode(void);

/* 延迟执行配网（3 击），给 5 击留机会 */
static void wifi_config_deferred_cb(void *arg)
{
    (void)arg;
    app_webserver_stop();
    dooi_enter_wifi_config_mode();
}

void on_power_btn_press(const app_event_t *evt)
{
    static uint8_t  press_count     = 0;
    static uint32_t last_press_tick = 0;
    static esp_timer_handle_t s_defer_timer = NULL;

    /* 关机充电状态下禁止单击操作：必须长按电源键（触发 on_power_shutdown）才能开机 */
    if (app_ui_logic_get_current_screen() == SCREEN_CHARGING_OFF) {
        ESP_LOGI(TAG, "Charging-off: single click ignored, long-press to power on");
        return;
    }

    uint32_t now = xTaskGetTickCount();

    /* 超过 3 秒无按键，重置计数并清理延迟定时器 */
    if (now - last_press_tick > pdMS_TO_TICKS(3000)) {
        press_count = 0;
        if (s_defer_timer) {
            esp_timer_stop(s_defer_timer);
            esp_timer_delete(s_defer_timer);
            s_defer_timer = NULL;
        }
    }

    press_count++;
    last_press_tick = now;

    /* ------------------------------------------------
     * 5 击 → 取消 3 击延迟，立即恢复出厂
     * ------------------------------------------------ */
    if (press_count == 5) {
        if (s_defer_timer) {
            esp_timer_stop(s_defer_timer);
            esp_timer_delete(s_defer_timer);
            s_defer_timer = NULL;
        }
        /* 恢复出厂提示音由 factory_reset 场景统一播放 */
        app_behavior_play_scene("factory_reset", NULL, BEHAVIOR_PRIO_DEFAULT);
        ESP_LOGW(TAG, "5-click detected, factory reset!");
        dooi_nvs_factory_reset();
        return;
    }

    /* ------------------------------------------------
     * 3 击 → 不立即执行，延迟 500ms 等第 4/5 击
     * ------------------------------------------------ */
    if (press_count == 3) {
        ESP_LOGW(TAG, "3-click detected, defer 500ms for possible 5-click...");
        if (!s_defer_timer) {
            const esp_timer_create_args_t args = {
                .callback = wifi_config_deferred_cb,
                .name = "wifi_cfg_defer",
            };
            esp_timer_create(&args, &s_defer_timer);
        }
        esp_timer_start_once(s_defer_timer, 500000); /* 500 ms */
        return;
    }

    /* 4 击 → 取消延迟，等待最终的第 5 击 */
    if (press_count == 4) {
        if (s_defer_timer) {
            esp_timer_stop(s_defer_timer);
        }
        return;
    }

    /* ------------------------------------------------
     * 1-2 次点击：保持原有行为
     * ------------------------------------------------ */
    if (app_servo_get_angle(SERVO_LEFT) != 180.0f || app_servo_get_angle(SERVO_RIGHT) != 180.0f) {
        app_behavior_status_set(BEHAVIOR_STATUS_IDLE);
    }

    app_event_type_t conv_state = app_ui_logic_get_state();
    if (conv_state == APP_EVT_STATE_SPEAKING || conv_state == APP_EVT_STATE_LISTENING) {
        ApplicationStopChat();
    }

    app_ble_trigger_xbox_scan();
}

void on_power_shutdown(const app_event_t *evt)
{
    (void)evt;

    if (app_ui_logic_get_current_screen() == SCREEN_CHARGING_OFF) {
        esp_restart();
        return;
    }

    /* 先停对话与行为，保证进入任何关机流程前机器人保持安静 */
    ApplicationStopChat();
    app_behavior_stop(PRIO_IDLE);
    app_behavior_status_set(BEHAVIOR_STATUS_IDLE);

    app_imu_enable_tilt_detect(false, 3.0f);
    app_imu_enable_push_detect(false, 0.3f);
    app_imu_enable_pickup_detect(false);

    app_motor_estop();
    app_led_off();

    /* 退出视觉处理（停止摄像头 / 视觉任务），进入关机/充电页面后不再识别 */
    app_event_t vision_exit = {.type = APP_EVT_VISION_TASK_EXITED};
    app_ui_logic_post(&vision_exit, false);

    /* 关机时若仍处于充电状态：不切断整机电源，进入"关机充电"页面。
     * 再次长按电源键（触发本函数）或充电拔出后再退出到眼睛界面。 */
    if (power_manager_is_charging() || power_manager_get_level() == 100) {
        ESP_LOGW(TAG, "Shutdown while charging, enter charging-off screen...");
        screen_show_wakeup_ack("SYS MSG", "系统即将关机", 50000);
        vTaskDelay(pdMS_TO_TICKS(2500));
        power_manager_power_off();
        app_ui_logic_request_screen(SCREEN_CHARGING_OFF);
        return;
    }

    ESP_LOGW(TAG, "Power shutdown requested, gracefully exiting...");

    screen_show_wakeup_ack("SYS MSG", "系统即将关机", 50000);

    /* 优雅退出：先停对话，再停所有行为，最后切断整机电源 */
    vTaskDelay(pdMS_TO_TICKS(2500));

    power_manager_power_off();
}

void on_power_status(const app_event_t *evt)
{
    bool is_low      = evt->data.power_status.low_battery;
    bool is_charging = evt->data.power_status.is_charging;

    /* 关机充电页面中检测到"停止充电"（充电被拔出）→ 用户之前已关机，直接断电关机 */
    if (!is_charging &&
        app_ui_logic_get_current_screen() == SCREEN_CHARGING_OFF) {
        ESP_LOGW(TAG, "Charging stopped while charging-off, shutting down...");
        power_manager_power_off();
        return;
    }

    if (is_low && !is_charging){
        ApplicationSendMessage("电池电量低了，要表达虚弱和提醒充电；比如：我好饿呀，电量快没了，快给我充充电吧～", true);
        ESP_LOGW(TAG, "Low battery!");
    }
}
