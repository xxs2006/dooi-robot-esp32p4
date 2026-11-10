#include "pages.h"
#include "app_ui_logic.h"

void logic_page_custom_on_key(int key_id, touch_btn_event_t event)
{
    /* 任意按键（单击 / 双击 / 长按）都立即返回上一页 */
    switch (event) {
        case TOUCH_BTN_EVT_CLICK:
        case TOUCH_BTN_EVT_DOUBLE_CLICK:
        case TOUCH_BTN_EVT_LONG_PRESS:
            custom_page_go_back();
            break;
        default:
            break;
    }
}
