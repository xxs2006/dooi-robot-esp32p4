#include "pages.h"
#include "app_ui_logic.h"

#include "esp_log.h"

static const char *TAG = "logic_page_dclock";

void logic_page_digital_clock_on_key(int key_id, touch_btn_event_t event)
{
    if (key_id == TOUCH_BTN_KEY_LEFT) {
        switch (event) {
            case TOUCH_BTN_EVT_CLICK:
                digital_clock_mode_change();
                ESP_LOGI(TAG, "Mode changed to: %d", digital_clock_mode_get());
                break;

            case TOUCH_BTN_EVT_DOUBLE_CLICK:
                if (digital_clock_mode_get() == MODE_TIMER) {
                    digital_timer_data_set(-5);
                    ESP_LOGI(TAG, "Timer decreased by 5 minutes");
                } else {
                    app_ui_logic_request_screen(SCREEN_EYES);
                }
                break;

            case TOUCH_BTN_EVT_LONG_PRESS:
                app_ui_logic_request_screen(SCREEN_EYES);
                break;

            default:
                break;
        }
    } else if (key_id == TOUCH_BTN_KEY_RIGHT) {
        switch (event) {
            case TOUCH_BTN_EVT_CLICK:
                digital_clock_ticking_toggle();
                break;

            case TOUCH_BTN_EVT_DOUBLE_CLICK:
                if (digital_clock_mode_get() == MODE_TIMER) {
                    digital_timer_data_set(5);
                    ESP_LOGI(TAG, "Timer increased by 5 minutes");
                } else {
                    app_ui_logic_request_screen(SCREEN_EYES);
                }
                break;

            case TOUCH_BTN_EVT_LONG_PRESS:
                app_ui_logic_request_screen(SCREEN_EYES);
                break;

            default:
                break;
        }
    }
}
