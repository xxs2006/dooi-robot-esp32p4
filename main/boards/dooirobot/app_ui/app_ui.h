#ifndef __APP_UI_H__
#define __APP_UI_H__

#include <stdbool.h>
#include "lvgl.h"
#include "app_periphral.h"
#include "app_vision.h"
#include "eyes_control.h"
#include "pages.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_UI_VERSION "Version:3.0.1"

/* UI 主循环心跳周期（ms） */
#define APP_UI_TICK_MS 25
#define APP_UI_MS_TO_TICKS(ms) ((ms) / APP_UI_TICK_MS)

/* EYES 页面待机超时（5 分钟）→ 进入 IDLE */
#define IDLE_TIMEOUT_MS 300000

/* 屏幕类型枚举（渲染层与逻辑层共用） */
typedef enum {
    SCREEN_NULL,
    SCREEN_DIGITAL_CLOCK,
    SCREEN_EMOJI_GIF,
    SCREEN_MUSIC_SPECTRUM,
    SCREEN_EYES,
    SCREEN_CAMERA,
    SCREEN_LUCKY_CAT,
    SCREEN_INFO,
    SCREEN_PAGE_SELECT,
    SCREEN_WAKEUP_ACK,
    SCREEN_CUSTOM_PAGE,
    SCREEN_WEATHER,
    SCREEN_CHARGING_OFF,
    SCREEN_COUNT
} screen_type_t;


/* 显示设备枚举 */
typedef enum {
    DISPLAY_LEFT,
    DISPLAY_RIGHT,
    DISPLAY_COUNT
} display_t;


/* 显示模式定义 */
typedef enum {
    MODE_TIME,  // 常规时间模式
    MODE_TIMER, // 番茄计时模式
    MODE_COUNT,
} digital_clock_mode_t;

typedef enum {
    SOUND_STOP,
    SOUND_1724636,
    SOUND_6812623,
    SOUND_1067002988,
    SOUND_1067936557,
    SOUND_1068702996,
    SOUND_TICKING_CLOCK,
    SOUND_COUNT
} SoundEffect_t;

/* 屏幕接口 */
typedef struct {
    int (*load)(void);
    int (*update)(void);
    int (*unload)(void);
    void (*on_key_event)(int key_id, touch_btn_event_t event);  
} screen_interface_t;

typedef enum {
    SCREEN_UNLOADED,
    SCREEN_LOADED
} screen_state_t;

/* 屏幕实例 */
typedef struct {
    lv_obj_t *root;
    bool needs_update;
    screen_interface_t *interface;
} screen_instance_t;

/* 屏幕管理器（渲染层自有状态；逻辑层通过 app_ui_logic_report_screen 获取镜像，
 * 不直接访问本结构体，避免跨线程共享可变状态） */
typedef struct {
    display_t current_display;
    screen_state_t screen_states[SCREEN_COUNT];
    screen_type_t current_screen;
    screen_type_t old_screen;
    screen_type_t next_screen;
    screen_instance_t screens[DISPLAY_COUNT];
    bool is_idle_timeout_triggered;
} screen_manager_t;

extern screen_manager_t screen_manager;

void screen_set_current(screen_type_t screen_type);
screen_type_t screen_get_current(void);
void screen_load_next(void);
void screen_idle_timeout_reset();
void app_ui_init(void);
uint32_t get_tick_ms(void);

void get_wifi_ip_string(char *buf, size_t buf_size);

/* XIAOZHI SYSTEM */
void ApplicationSendMessage(const char* message, bool use_tool);
void ApplicationPlaySound(SoundEffect_t sound);
void ApplicationStopChat();

void app_codec_gain_set(float gain);

void dooi_set_volume(int volume);

#ifdef __cplusplus
}
#endif

#endif