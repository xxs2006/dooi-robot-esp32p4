#include "pages.h"
#include "app_ui_logic.h"

void logic_page_lucky_cat_on_key(int key_id, touch_btn_event_t event)
{
    if (key_id == TOUCH_BTN_KEY_LEFT) {
        switch (event) {
            case TOUCH_BTN_EVT_CLICK:
                lucky_cat_style_switch(-1);
                break;
            case TOUCH_BTN_EVT_DOUBLE_CLICK:
                app_ui_logic_request_screen(SCREEN_PAGE_SELECT);
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
                lucky_cat_style_switch(1);
                break;
            case TOUCH_BTN_EVT_DOUBLE_CLICK:
                app_ui_logic_request_screen(SCREEN_PAGE_SELECT);
                break;
            case TOUCH_BTN_EVT_LONG_PRESS:
                app_ui_logic_request_screen(SCREEN_EYES);
                break;
            default:
                break;
        }
    }
}
