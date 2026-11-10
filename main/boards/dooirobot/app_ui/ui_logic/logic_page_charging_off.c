// logic_page_charging_off.c  —— 关机充电页面按键逻辑
//
//  本页面处于"关机充电"状态：不允许任何触摸操作，因此 on_key 一律忽略。
//  退出本页面的唯一途径由外部触发：
//    - 用户按下电源键开机（logic_sys_status 中处理）
//    - 充电拔出，停止充电（on_power_status 中处理）
//
#include "pages.h"
#include "app_ui_logic.h"
#include "esp_log.h"

static const char *TAG = "logic_charging_off";

void logic_page_charging_off_on_key(int key_id, touch_btn_event_t event)
{
    /* 关机充电状态下禁止一切触摸操作 */
    (void)key_id;
    (void)event;
    ESP_LOGI(TAG, "Charging-off: input ignored");
}

/* 退出关机充电界面，默认回到眼睛界面 */
void charging_off_exit_to_eyes(void)
{
    ESP_LOGI(TAG, "Charging off screen exit, go to eyes");
    app_ui_logic_request_screen(SCREEN_EYES);
}
