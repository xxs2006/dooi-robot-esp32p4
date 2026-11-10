#include "app_ui.h"
#include "display_hal.h"
#include "eyes_control.h"
#include "face_tracker.h"
#include "app_behavior.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "app_ui_logic.h"

static const char *TAG = "app_ui";

/* 屏幕管理器实例 */
screen_manager_t screen_manager = {0};

/* 外部屏幕接口声明 */
extern screen_interface_t eyes_screen_interface;
extern screen_interface_t digital_clock_screen_interface;
extern screen_interface_t music_screen_interface;
extern screen_interface_t camera_screen_interface;
extern screen_interface_t lucky_cat_screen_interface;
extern screen_interface_t info_settings_screen_interface;
extern screen_interface_t wakeup_ack_screen_interface;
extern screen_interface_t page_select_screen_interface;
extern screen_interface_t custom_page_screen_interface;
extern screen_interface_t weather_screen_interface;
extern screen_interface_t charging_off_screen_interface;

static screen_interface_t *screen_interfaces[SCREEN_COUNT] = {
    [SCREEN_DIGITAL_CLOCK] = &digital_clock_screen_interface,
    [SCREEN_EMOJI_GIF]     = NULL,
    [SCREEN_MUSIC_SPECTRUM]= &music_screen_interface,
    /* 单屏双眼：眼睛引擎已改成「一块屏里并排画两只眼」，SCREEN_EYES 恢复正常 */
    [SCREEN_EYES]          = &eyes_screen_interface,
    [SCREEN_CAMERA]        = &camera_screen_interface,
    [SCREEN_LUCKY_CAT]     = &lucky_cat_screen_interface,
    [SCREEN_INFO]          = &info_settings_screen_interface,
    [SCREEN_PAGE_SELECT]   = &page_select_screen_interface,
    [SCREEN_WAKEUP_ACK]    = &wakeup_ack_screen_interface,
    [SCREEN_CUSTOM_PAGE]   = &custom_page_screen_interface,
    [SCREEN_WEATHER]       = &weather_screen_interface,
    [SCREEN_CHARGING_OFF]  = &charging_off_screen_interface,
};

uint32_t get_tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool is_direct_render_screen(screen_type_t id)
{
    /* 眼睛页是唯一直写 panel 的页面（raw flush，不经 LVGL）。
     * 它必须跳过 lv_scr_load，否则 LVGL 会把默认空屏刷上去盖掉眼睛。 */
    return (id == SCREEN_EYES);
}

static void screen_switch_display(display_t display_id)
{
    display_dev_t *dev = display_get_dev(display_id);
    lv_disp_set_default(dev->lv_disp);
    screen_manager.current_display = display_id;
}

void screen_set_current(screen_type_t screen_type)
{
    screen_manager.next_screen = screen_type;
}

screen_type_t screen_get_current(void)
{
    return screen_manager.current_screen;
}

void screen_init(void)
{
    screen_switch_display(DISPLAY_LEFT);
    screen_manager.screens[DISPLAY_LEFT].root = lv_obj_create(NULL);
    screen_switch_display(DISPLAY_RIGHT);
    screen_manager.screens[DISPLAY_RIGHT].root = lv_obj_create(NULL);
}

void screen_load_next(void)
{
    int screen_type = screen_manager.current_screen;

    do {
        screen_type++;
        if (screen_type >= SCREEN_COUNT) {
            screen_type = SCREEN_DIGITAL_CLOCK;
        }
    } while (screen_interfaces[screen_type] == NULL && screen_type != SCREEN_DIGITAL_CLOCK);

    screen_set_current(screen_type);
}

void screen_load(screen_type_t screen_type)
{
    int current_screen = screen_manager.current_screen;

    if (screen_type >= SCREEN_COUNT || screen_type == SCREEN_NULL) {
        return;
    }

    if (current_screen != SCREEN_NULL && screen_interfaces[current_screen] != NULL) {
        screen_manager.screen_states[current_screen] = SCREEN_UNLOADED;
        ESP_LOGI(TAG, "unload screen %d", current_screen);
        screen_interfaces[current_screen]->unload();
        if (screen_manager.screens[DISPLAY_LEFT].root) {
            lv_obj_clean(screen_manager.screens[DISPLAY_LEFT].root); 
        }
        if (screen_manager.screens[DISPLAY_RIGHT].root) {
            lv_obj_clean(screen_manager.screens[DISPLAY_RIGHT].root);
        }
    }

    ESP_LOGI(TAG, "load screen %d", screen_type);

    if (screen_interfaces[screen_type] != NULL) {
        screen_interfaces[screen_type]->load();
    }

    screen_manager.screen_states[screen_type] = SCREEN_LOADED;

    if (!is_direct_render_screen(screen_type)) {
        if (screen_manager.screens[DISPLAY_LEFT].root != NULL) {
            lv_scr_load(screen_manager.screens[DISPLAY_LEFT].root);
        }
        if (screen_manager.screens[DISPLAY_RIGHT].root != NULL) {
            lv_scr_load(screen_manager.screens[DISPLAY_RIGHT].root);
        }
        lv_timer_handler();
    }
}

void screen_update(void)
{
    if (screen_manager.next_screen != SCREEN_NULL) {
        if (screen_manager.current_screen != screen_manager.next_screen) {
            screen_manager.old_screen = screen_manager.current_screen;
            screen_load(screen_manager.next_screen);
            screen_manager.current_screen = screen_manager.next_screen;
            
            app_ui_logic_report_screen(screen_manager.current_screen, screen_manager.old_screen);
        }
        screen_manager.next_screen = SCREEN_NULL;
    }

    int current_screen = screen_manager.current_screen;
    screen_interface_t *screen = screen_interfaces[current_screen];

    if (!screen || !screen->update) {
        return;
    }

    if (screen_manager.screen_states[current_screen] == SCREEN_LOADED) {
        screen->update();
    }
}

void screen_idle_timeout_reset(void)
{
    if (screen_manager.is_idle_timeout_triggered) {
        screen_set_current(SCREEN_EYES);
        screen_manager.is_idle_timeout_triggered = false;
    }
}

void screen_idle_timeout_trigger(void)
{
    screen_manager.is_idle_timeout_triggered = true;
}

void app_dooi_ui(void *arg)
{
    display_hal_init();

    /* ---- 眼睛控制器必须初始化，否则任何表情/动作都会崩溃重启 ----
     * little_eyes 库的 eye_controller_* 系列（表情、眼位、瞳孔、虹膜、眨眼）
     * 操作的都是库内一个全局单例，该单例由 eye_controller_init() 创建，
     * 而 eye_controller_init() 只在 eyes_render_init() 里被调用。
     *
     * 触发链：app_behavior -> logic_emotion_set_manual -> eye_controller_set_emotion
     *        -> eye_controller_apply_preset -> eye_controller_set_target_position_eye
     *        -> 对 NULL+0x64 解引用 -> Load access fault -> 重启
     * 现象：开机约 22 秒，一说句话/一有动作就 panic 重启，无限循环。
     *
     * 这里恢复调用是安全的：它只做「安装显示后端 + 初始化控制器 + 创建两个渲染
     * 任务并立即挂起」。渲染任务受 s_eyes_running 控制，该标志默认 false，
     * 只有眼睛页 load 时才置 true，而测试模式从不加载眼睛页，
     * 所以不会去写面板、不会盖掉测试画面。 */
    eyes_render_init();

#if DISP_TEST_MODE
    /* ---- 测试模式：只验证屏幕硬件与驱动 ----
     * 不创建 LVGL 显示对象、不启动眼睛渲染、不加载任何页面，
     * 屏幕上的画面不会被别的逻辑覆盖，现象最干净、最好判断。
     * 这里只调 lv_init()：其他模块（人脸追踪）会调用 lv_tick_get()，
     * 完全不初始化 LVGL 会读到未定义状态。 */
    lv_init();
    ESP_LOGW(TAG, "DISP_TEST_MODE=1，进入屏幕测试画面（不启动 UI）");
    display_test_pattern_run();   /* 内部死循环，正常不返回 */
#endif

    /* ---- 正常模式：单屏 LVGL 界面 ---- */
    lvgl_displays_init();

    screen_init();
    /* 单屏双眼改造：开机直接进眼睛页（原来的开屏动画页是按 240x240 双屏排版的，
     * 单屏下会错位，先跳过它）。 */
    screen_set_current(SCREEN_EYES);

    while (1) {
        screen_update();
        app_ui_logic_tick();
        app_tip_led_run();
        face_tracker_update();
        vTaskDelay(pdMS_TO_TICKS(APP_UI_TICK_MS));
    }
}

void app_ui_init(void)
{
    xTaskCreatePinnedToCore(app_dooi_ui, "dooi_ui", 8192, (void *)1, 2, NULL, 1);
}