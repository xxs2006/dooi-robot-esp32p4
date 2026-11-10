#include <stdio.h>

#include "logic_peripheral.h"

#include "app_periphral.h"
#include "app_ui.h"
#include "app_ui_logic.h"
#include "app_motor.h"
#include "dooi_nvs.h"
#include "dooi_behavior.h"
#include "app_behavior.h"
#include "face_tracker.h"
#include "eyes_control.h"
#include "app_behavior_status.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "logic_periph";

/* -----------------------------------------------------------------------
 * 相机预览触发
 * --------------------------------------------------------------------- */
void on_camera_trigger_preview(const app_event_t *evt)
{
    camera_request_t req = {.mode = CAMERA_MODE_PHOTO, .trigger = CAMERA_TRIGGER_NONE};
    camera_trigger_funtion(&req);
}

/* -----------------------------------------------------------------------
 * 红外避障
 * --------------------------------------------------------------------- */
void on_ir_enable(const app_event_t *evt)
{
    ir_enable();
    vTaskDelay(pdMS_TO_TICKS(50));
    ir_unmute();
}

void on_ir_disable(const app_event_t *evt)
{
    ir_mute();
    ir_disable();
}

void on_ir_trigger(const app_event_t *evt)
{
    if (app_motor_is_active()) {
        // ApplicationSendMessage("检测到部分机身悬空", false);
        app_motor_estop();
        if (evt->data.ir_data.sensor_id == IR_SENSOR_3 ||
            evt->data.ir_data.sensor_id == IR_SENSOR_4) {
            app_motor_move_auto(-100, -100, 200);
        } else {
            app_motor_move_auto(100, 100, 200);
        }
    }
    ESP_LOGW(TAG, "IR Triggered: %d\n", evt->data.ir_data.sensor_id);
}

/* -----------------------------------------------------------------------
 * IMU 姿态 / 倾斜 / 推动 / 抱起
 * --------------------------------------------------------------------- */
void on_imu(const app_event_t *evt)
{
    static uint8_t imu_push_count = 0;
    static uint32_t first_push_tick = 0;

    imu_tilt_dir_t dir = evt->data.imu_data.dir;
    ESP_LOGW(TAG, "IMU Tilt Detected! Dir: %d", dir);

    if (app_motor_is_active()) {
        return;
    }

    switch (dir) {
    case IMU_TILT_RIGHT:   // 右方 — 音量+（已注释：倾斜不再控制音量）
    {
        /* 倾斜调音量逻辑已注释掉
        int volume = 70;
        dooi_nvs_get_volume(&volume);
        volume += 10;
        if (volume > 100) volume = 100;
        dooi_nvs_set_volume(volume);
        dooi_set_volume(volume);

        char notify_buf[32];
        snprintf(notify_buf, sizeof(notify_buf), "音量 + : %d%%", volume);
        screen_show_wakeup_ack("SYS MESSAGE", notify_buf, APP_UI_MS_TO_TICKS(1500));
        */
        app_servo_set_angle(SERVO_TARGET_RIGHT, 180.0f, 1000);
        break;
    }
    case IMU_TILT_LEFT:    // 左方 — 音量-（已注释：倾斜不再控制音量）
    {
        /* 倾斜调音量逻辑已注释掉
        int volume = 70;
        dooi_nvs_get_volume(&volume);
        volume -= 10;
        if (volume < 0) volume = 0;
        dooi_nvs_set_volume(volume);
        dooi_set_volume(volume);

        char notify_buf[32];
        snprintf(notify_buf, sizeof(notify_buf), "音量 - : %d%%", volume);
        screen_show_wakeup_ack("SYS MESSAGE", notify_buf, APP_UI_MS_TO_TICKS(1500));
        */
        app_servo_set_angle(SERVO_TARGET_LEFT, 180.0f, 1000);
        break;
    }
    case IMU_TILT_BACK:    // 前方
        // ApplicationSendMessage("你好呀", false);
        break;
    case IMU_ENV_PUSH: {
        uint32_t now = get_tick_ms();

        /* 被推动触发：进入待机态（app_behavior_status_set 内部已防止重复进入） */
        app_behavior_status_set(BEHAVIOR_STATUS_STANDBY);

        if (imu_push_count == 0) {
            /* 第一次被推：灯光闪烁；声音由 action_pushed_flash 场景的 audio 通道统一播放 */
            first_push_tick = now;
            imu_push_count = 1;
            app_behavior_play_scene("action_pushed_flash", NULL, BEHAVIOR_PRIO_DEFAULT);
        } else {
            if ((now - first_push_tick) <= 6000) {
                imu_push_count++;

                if (imu_push_count >= 3) {
                    ApplicationSendMessage("你被多次频繁推动，要表达生气和不耐烦；比如：别推了别推了！再推我就要生气了！", true);
                    imu_push_count = 0;
                    first_push_tick = 0;
                } else if (imu_push_count == 2) {
                    /* 第二次被推：生气动作；声音由 angry 场景的 audio 通道统一播放 */
                    app_behavior_play_scene("angry", NULL, BEHAVIOR_PRIO_DEFAULT);
                }
            } else {
                // 超过6秒，重新开始计数；声音由 action_pushed_flash 场景的 audio 通道统一播放
                first_push_tick = now;
                imu_push_count = 1;
                app_behavior_play_scene("action_pushed_flash", NULL, BEHAVIOR_PRIO_DEFAULT);
            }
        }
        break;
    }
    case IMU_PICKUP:
        app_ui_logic_request_screen(SCREEN_EYES);
        eye_controller_apply_emotion(EYES_EMOTION_SUSPICIOUS);
        app_servo_set_target_angle(SERVO_TARGET_BOTH, 180.0f, 1000);
        app_led_set_blink(LED_COLOR_RED, 500, true);
        ApplicationSendMessage("你被拿起来了，要表达恐高和害怕；比如：哎呀呀，太高啦太高啦，我有恐高症，快放我下来！", true);
        break;
    case IMU_PICKDOWN:
        app_ui_logic_request_screen(SCREEN_EYES);
        eye_controller_apply_emotion(EYES_EMOTION_HAPPY);
        app_servo_set_target_angle(SERVO_TARGET_BOTH, 135.0f, 1000);
        app_led_off();
        break;

    default:
        break;
    }
}

/* -----------------------------------------------------------------------
 * 按键事件：集中处理"纯 UI 交互逻辑"
 *   - 双长按（左右键同时长按）检测 → 进入页面选择
 *   - 长按计时清理（避免陈旧时间戳干扰双长按判定）
 *   - 按当前屏幕把按键事件分发到对应 logic_page_*_on_key
 * 原 app_ui.c 的 screen_ui_control / check_dual_long_press 已迁移至此。
 * --------------------------------------------------------------------- */
#define DUAL_LONG_PRESS_WINDOW_MS  1000
#define LONG_PRESS_STALE_MS        2000

/* 按键输入状态：逻辑层私有，不再依赖渲染层 screen_manager */
static struct {
    uint32_t left_long_press_time;
    uint32_t right_long_press_time;
} s_key_state = {0, 0};

static bool dual_long_press_check(void)
{
    uint32_t left_time  = s_key_state.left_long_press_time;
    uint32_t right_time = s_key_state.right_long_press_time;

    if (left_time == 0 || right_time == 0) {
        return false;
    }

    uint32_t time_diff = (left_time > right_time) ?
                         (left_time - right_time) :
                         (right_time - left_time);

    if (time_diff <= DUAL_LONG_PRESS_WINDOW_MS) {
        s_key_state.left_long_press_time  = 0;
        s_key_state.right_long_press_time = 0;
        return true;
    }

    return false;
}

void on_button(const app_event_t *evt)
{
    int key_id = evt->data.btn_data.btn_id;
    touch_btn_event_t event = evt->data.btn_data.event;

    ESP_LOGI(TAG, "on_button: key_id %d, event %d", key_id, event);

    /* 按键触发：进入待机态。已处于 STANDBY 则不再执行状态切换与事件投递，
     * 避免每次按键都重复走 standby 逻辑（状态/事件均已去重）。 */
    if (app_behavior_status_current() != BEHAVIOR_STATUS_STANDBY) {
        app_behavior_status_set(BEHAVIOR_STATUS_STANDBY);
    }

    if (event == TOUCH_BTN_EVT_LONG_PRESS) {
        uint32_t now = get_tick_ms();

        if (key_id == TOUCH_BTN_KEY_LEFT) {
            s_key_state.left_long_press_time = now;
        } else if (key_id == TOUCH_BTN_KEY_RIGHT) {
            s_key_state.right_long_press_time = now;
        }

        if (dual_long_press_check()) {
            ESP_LOGI(TAG, "Dual long press detected, entering page select");
            app_ui_logic_request_screen(SCREEN_PAGE_SELECT);
            return;
        }
    }

    /* 长按时间戳超过 2s 未配对则清除，避免干扰后续双长按判定 */
    uint32_t now = get_tick_ms();
    if (s_key_state.left_long_press_time > 0 &&
        (now - s_key_state.left_long_press_time) > LONG_PRESS_STALE_MS) {
        s_key_state.left_long_press_time = 0;
    }
    if (s_key_state.right_long_press_time > 0 &&
        (now - s_key_state.right_long_press_time) > LONG_PRESS_STALE_MS) {
        s_key_state.right_long_press_time = 0;
    }

    /* 按当前屏幕分发到对应页面按键逻辑（logic_page_xxx.c） */
    switch (app_ui_logic_get_current_screen()) {
    case SCREEN_DIGITAL_CLOCK: logic_page_digital_clock_on_key(key_id, event); break;
    case SCREEN_EYES:          logic_page_eyes_on_key(key_id, event);          break;
    case SCREEN_MUSIC_SPECTRUM:logic_page_music_on_key(key_id, event);         break;
    case SCREEN_CAMERA:        logic_page_camera_on_key(key_id, event);        break;
    case SCREEN_LUCKY_CAT:     logic_page_lucky_cat_on_key(key_id, event);     break;
    case SCREEN_INFO:          logic_page_info_on_key(key_id, event);          break;
    case SCREEN_PAGE_SELECT:   logic_page_page_select_on_key(key_id, event);   break;
    case SCREEN_WAKEUP_ACK:    logic_page_wakeup_ack_on_key(key_id, event);    break;
    case SCREEN_CUSTOM_PAGE:   logic_page_custom_on_key(key_id, event);        break;
    case SCREEN_WEATHER:       logic_page_weather_on_key(key_id, event);       break;
    case SCREEN_CHARGING_OFF:  logic_page_charging_off_on_key(key_id, event);  break;
    default:                   break;
    }
}
