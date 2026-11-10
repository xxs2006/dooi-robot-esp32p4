#include "pages.h"
#include "app_ui_logic.h"

void logic_page_info_on_key(int key_id, touch_btn_event_t event)
{
    if (key_id == TOUCH_BTN_KEY_LEFT || key_id == TOUCH_BTN_KEY_RIGHT) {
        switch (event) {
            case TOUCH_BTN_EVT_DOUBLE_CLICK:
                info_toggle_ble();
                break;
            case TOUCH_BTN_EVT_LONG_PRESS:
                app_ui_logic_request_screen(SCREEN_EYES);
                break;
            default:
                break;
        }
    }
}
