#ifndef __PAGES_H__
#define __PAGES_H__

#include "app_ui.h"
#include "app_vision.h"
#include "eyes_control.h"

#ifdef __cplusplus
extern "C" {
#endif

// 摄像头模式定义
typedef enum {
    CAMERA_MODE_NULL = 0,
    CAMERA_MODE_PREVIEW,
    CAMERA_MODE_PHOTO,
    CAMERA_MODE_FACE_DETECT,
    CAMERA_MODE_GESTURE_RECOGNIZE,
    CAMERA_MODE_MAX
} camera_mode_t;

/* 摄像头跳转请求：描述进入 SCREEN_CAMERA 时的完整意图 */
typedef enum {
    CAMERA_TRIGGER_NONE = 0,        // 不额外触发，仅切模式
    CAMERA_TRIGGER_FACE_ALIGN_ONCE, // 单次对齐后发 ALIGNED 事件
    CAMERA_TRIGGER_FACE_TRACK_LOOP, // 持续跟随
} camera_trigger_t;

typedef struct {
    camera_mode_t   mode;    // 进入后的初始模式
    camera_trigger_t trigger; // 进入后的触发动作
} camera_request_t;

/* -----------------------------------------------------------------------
 * 各 page 对外暴露的接口集中声明。
 *
 * 之前这些声明散落在 app_ui.h 中，随着按键逻辑迁移到 ui_logic/logic_page_*
 * 后数量膨胀。现统一收敛到本文件，使 app_ui.h 只保留屏幕管理核心，
 * 各 page 与 logic_page_* 模块直接 include 本头即可。
 * --------------------------------------------------------------------- */

/* -----------------------------------------------------------------------
 * page 按键动作接口（供 ui_logic/logic_page_xxx 调用）
 * --------------------------------------------------------------------- */

/* 数字时钟：切换走秒(滴答)音效开关 */
void digital_clock_ticking_toggle(void);

/* 招财猫：风格上一/下一个（delta = -1 上, +1 下） */
void lucky_cat_style_switch(int delta);

/* 相机：读取当前模式 */
camera_mode_t camera_mode_get(void);

/* 信息/设置：切换蓝牙开关 */
void info_toggle_ble(void);

/* 自定义页：返回进入前的页面 */
void custom_page_go_back(void);

/* 页面选择器：移动选中项 / 确认进入 */
void page_select_move(int dir);
void page_select_confirm(void);

/* 眼睛：处理按键（内部手势引擎已迁至 logic_page_eyes，入口即 logic_page_eyes_on_key） */

/* 眼睛：渲染侧查询/重置手势游戏状态（内部引擎已迁至 logic_page_eyes） */
void logic_page_eyes_reset_state(void);
bool logic_page_eyes_is_game_playing(void);
uint32_t logic_page_eyes_game_round_start_time(void);
void logic_page_eyes_game_timeout_check(void);

/* -----------------------------------------------------------------------
 * page 数据 / 功能接口
 * --------------------------------------------------------------------- */

/* 数字时钟 / 番茄钟 */
void digital_clock_mode_set(int mode);
int  digital_clock_mode_get(void);
void digital_clock_mode_change(void);
void digital_timer_set_minutes(int mins);
void digital_timer_data_set(int mins);
void digital_timer_reset(void);

/* 招财猫风格设置（需在 lucky_cat 屏幕中实现） */
void lucky_cat_style_set(int style);

/* 摄像头 */
void camera_on_vision_result_to_screen(vision_mode_t model_type, const vision_result_t* results, size_t count, void* user_data);
void camera_trigger_funtion(const camera_request_t *req);
void camera_mode_switch(camera_mode_t new_mode);

/* 天气页面数据接口 */
void weather_screen_set_data(const char *location, const char *condition_zh, int temperature);

/* Wakeup ACK 页面：一次性设置主题、通知内容与自动熄屏超时（ticks） */
void screen_show_wakeup_ack(const char* topic, const char* notification, int timeout_ticks);
void screen_idle_timeout_trigger(void);

/* -----------------------------------------------------------------------
 * 按键事件处理逻辑（logic_page_xxx 模块）统一入口声明。
 * 各 page 的 screen_interface_t.on_key_event 直接指向以下函数，
 * 按键逻辑已迁移到 ui_logic/logic_page_xxx.c，page 只保留显示逻辑。
 * --------------------------------------------------------------------- */
void logic_page_digital_clock_on_key(int key_id, touch_btn_event_t event);
void logic_page_camera_on_key(int key_id, touch_btn_event_t event);
void logic_page_lucky_cat_on_key(int key_id, touch_btn_event_t event);
void logic_page_eyes_on_key(int key_id, touch_btn_event_t event);
void logic_page_page_select_on_key(int key_id, touch_btn_event_t event);
void logic_page_music_on_key(int key_id, touch_btn_event_t event);
void logic_page_weather_on_key(int key_id, touch_btn_event_t event);
void logic_page_info_on_key(int key_id, touch_btn_event_t event);
void logic_page_custom_on_key(int key_id, touch_btn_event_t event);
void logic_page_wakeup_ack_on_key(int key_id, touch_btn_event_t event);
void logic_page_charging_off_on_key(int key_id, touch_btn_event_t event);
void charging_off_exit_to_eyes(void);

#ifdef __cplusplus
}
#endif

#endif /* __PAGES_H__ */
