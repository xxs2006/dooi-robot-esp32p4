#include "pages.h"
#include "app_ui_logic.h"
#include "esp_timer.h"

void logic_page_wakeup_ack_on_key(int key_id, touch_btn_event_t event)
{
    static int64_t s_first_click_time_us = 0;
    static uint8_t s_click_count = 0;

    bool should_handle_special_event = false;

    if (event == TOUCH_BTN_EVT_DOUBLE_CLICK) {
        /*
         * 双击直接触发特殊事件处理；双击后清空单击统计
         */
        should_handle_special_event = true;
        s_first_click_time_us = 0;
        s_click_count = 0;
    } else if (event == TOUCH_BTN_EVT_CLICK) {
        int64_t now_us = esp_timer_get_time();

        /*
         * 第一次单击，或距首次单击已超过 2s，重新开始统计
         */
        if (s_first_click_time_us == 0 ||
            now_us - s_first_click_time_us > 2 * 1000 * 1000) {
            s_first_click_time_us = now_us;
            s_click_count = 1;
        } else {
            s_click_count++;
        }

        /*
         * 2s 内超过 2 次单击，即第 3 次单击开始触发
         */
        if (s_click_count > 2) {
            should_handle_special_event = true;
            s_first_click_time_us = 0;
            s_click_count = 0;
        }
    } else {
        return;
    }

    if (should_handle_special_event == false) {
        return;
    }

    if (event == TOUCH_BTN_EVT_DOUBLE_CLICK) {
        if (key_id == TOUCH_BTN_KEY_RIGHT) {
            if (app_ui_logic_is_init_done() == false) {
                // dooi_nvs_factory_reset();
                return;
            }
        } else if (key_id == TOUCH_BTN_KEY_LEFT) {
            if (app_ui_logic_is_init_done() == false &&
                app_ui_logic_get_state() == APP_EVT_STATE_CONNECT_WIFI) {
                // dooi_enter_wifi_config_mode();
                return;
            }
        }
    }
}
