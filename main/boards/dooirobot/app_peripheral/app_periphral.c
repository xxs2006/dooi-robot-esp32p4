#include "app_periphral.h"
#include "touch_button.h"
#include "app_ui.h"
#include "app_ui_logic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char* TAG = "APP_PERIPHRAL";


/* ── 事件上报回调 ────────────────────────────────────── */
void ir_stop_handler(ir_sensor_id_t sensor_id)
{
    app_event_t evt = { .type = APP_EVT_IR_TRIGGER,
                        .data = { .ir_data = { .sensor_id = sensor_id } } };
    app_ui_logic_post(&evt, true);
}

static void on_imu_tilt(imu_tilt_dir_t dir, void *arg)
{
    app_event_t evt = { .type = APP_EVT_IMU,
                        .data = { .imu_data = { .dir = dir } } };
    app_ui_logic_post(&evt, false);
}

static void on_imu_push(void *arg)
{
    if (app_motor_is_active()) return;
    app_event_t evt = { .type = APP_EVT_IMU,
                        .data = { .imu_data = { .dir = IMU_ENV_PUSH } } };
    app_ui_logic_post(&evt, false);
}

static void on_imu_lift(bool is_picked_up, void *arg)
{
    if(is_picked_up)
    {
        app_event_t evt = { .type = APP_EVT_IMU,
                        .data = { .imu_data = { .dir = is_picked_up ? IMU_PICKUP : IMU_PICKDOWN } } };
        app_ui_logic_post(&evt, false);
    }
    else
    {
        app_event_t evt = { .type = APP_EVT_IMU,
                        .data = { .imu_data = { .dir = is_picked_up ? IMU_PICKUP : IMU_PICKDOWN } } };
        app_ui_logic_post(&evt, false);
    }
}

static void on_charging(bool is_charging)
{
    app_event_t evt = { .type = APP_EVT_POWER_STATUS,
                        .data = { .power_status = { .is_charging = is_charging } } };
    app_ui_logic_post(&evt, true);
}

static void on_low_battery(bool is_low)
{
    app_event_t evt = { .type = APP_EVT_POWER_STATUS,
                        .data = { .power_status = { .low_battery = is_low } } };
    app_ui_logic_post(&evt, true);
}

/* 按键是纯 UI 交互：投递到逻辑层，由 on_button 统一处理双长按/分发 */
static void on_btn(int key_id, touch_btn_event_t event, void *user_ctx)
{
    app_event_t evt = {
        .type = APP_EVT_BUTTON,
        .data = { .btn_data = { .btn_id = key_id, .event = event } }
    };
    app_ui_logic_post(&evt, false);
}

static void IRAM_ATTR on_power_btn(void)
{
    app_event_t evt = { .type = APP_EVT_POWER_BTN_PRESS };
    app_ui_logic_post(&evt, true);
}

static void periphral_on_power_shutdown(void)
{
    app_event_t evt = { .type = APP_EVT_POWER_SHUTDOWN };
    app_ui_logic_post(&evt, false);
}

void app_tip_led_run(void)
{
    static int8_t s_lvl = 0, s_tick = 0;
    if (++s_tick >= APP_UI_MS_TO_TICKS(1000)) {
        s_tick = 0;
        gpio_set_level(52, (s_lvl ^= 1));
    }
}

/* ── 初始化 ──────────────────────────────────────────── */
void app_periphral_init(void)
{
    app_servo_init();
    app_led_init();

    app_imu_init();
    app_imu_set_tilt_callback(on_imu_tilt, NULL);
    app_imu_enable_tilt_detect(true, 3.0f);
    app_imu_set_push_callback(on_imu_push, NULL);
    app_imu_enable_push_detect(true, 0.3f);
    app_imu_set_pickup_callback(on_imu_lift, NULL);
    app_imu_enable_pickup_detect(true);

    app_motor_init();
    ir_init(ir_stop_handler);

    static const power_config_t pwr_cfg = {
        .on_charging_changed    = on_charging,
        .on_low_battery_changed = on_low_battery,
        .on_power_btn_pressed   = on_power_btn,
        .on_power_shutdown      = periphral_on_power_shutdown,
    };
    power_manager_init(&pwr_cfg);

    /* 触摸：init 内部完成 NVS 读取、通道探测、校准 */
    ESP_ERROR_CHECK(touch_button_init());
    ESP_ERROR_CHECK(touch_button_register_callback(TOUCH_BTN_EVT_CLICK,        on_btn, NULL));
    ESP_ERROR_CHECK(touch_button_register_callback(TOUCH_BTN_EVT_DOUBLE_CLICK, on_btn, NULL));
    ESP_ERROR_CHECK(touch_button_register_callback(TOUCH_BTN_EVT_LONG_PRESS,   on_btn, NULL));
    ESP_ERROR_CHECK(touch_button_start());

    /* Tip LED GPIO */
    static const gpio_config_t io = {
        .pin_bit_mask = (1ULL << 52),
        .mode         = GPIO_MODE_OUTPUT,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}