#include "pages.h"
#include "app_ui_logic.h"

void logic_page_page_select_on_key(int key_id, touch_btn_event_t event)
{
    if (key_id == TOUCH_BTN_KEY_LEFT) {
        switch (event) {
            case TOUCH_BTN_EVT_CLICK:
                page_select_move(-1);
                break;
            case TOUCH_BTN_EVT_DOUBLE_CLICK:
                page_select_confirm();
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
                page_select_move(1);
                break;
            case TOUCH_BTN_EVT_DOUBLE_CLICK:
                page_select_confirm();
                break;
            case TOUCH_BTN_EVT_LONG_PRESS:
                app_ui_logic_request_screen(SCREEN_EYES);
                break;
            default:
                break;
        }
    }
}
