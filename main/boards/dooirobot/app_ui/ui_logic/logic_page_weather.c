#include "pages.h"
#include "app_ui_logic.h"

void logic_page_weather_on_key(int key_id, touch_btn_event_t event)
{
    /*
     * 任意按键单击：回到时钟；长按：跳转到眼睛页
     */
    if (event == TOUCH_BTN_EVT_CLICK) {
        app_ui_logic_request_screen(SCREEN_DIGITAL_CLOCK);
    } else if (event == TOUCH_BTN_EVT_LONG_PRESS) {
        app_ui_logic_request_screen(SCREEN_EYES);
    }
}
