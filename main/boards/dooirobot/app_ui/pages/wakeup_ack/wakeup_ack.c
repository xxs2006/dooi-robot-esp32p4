#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <lvgl.h>
#include "app_ui.h"
#include "dooi_nvs.h"

#include "app_ui_logic.h"


#define MAX_NOTIFICATION_LEN 256
#define MAX_ACK_TOPIC_LEN    48

/* 字体定义 */
LV_FONT_DECLARE(lv_font_notosans_cs_medium_14);

/* 统一风格 */
#define PAGE_BG_COLOR          0x000000
#define PANEL_BORDER_COLOR     0x3B82F6
#define TEXT_PRIMARY_COLOR     0xFFFFFF
#define TEXT_SECONDARY_COLOR   0xA0A0A0

static SemaphoreHandle_t s_wakeup_ack_mutex = NULL;

void dooi_enter_wifi_config_mode(void);

/* Wakeup ACK 屏幕私有数据 */
typedef struct {
    /* 左屏元素 */
    lv_obj_t *left_obj;           // 左屏圆形容器
    lv_obj_t *left_label;         // ACK主题文本

    /* 右屏元素 */
    lv_obj_t *right_obj;          // 右屏圆形容器
    lv_obj_t *right_label;        // 通知文本

    /* 数据缓存 */
    char ack_topic[MAX_ACK_TOPIC_LEN];
    char notification[MAX_NOTIFICATION_LEN];

    /* 状态标志 */
    bool needs_refresh;

    /* 本地超时管理（由本模块自行计时并退回到上一页） */
    int  timeout_target_tick;  // 0 表示不超时
    int  timeout_tick;         // 当前已累计 tick
} wakeup_ack_data_t;

static wakeup_ack_data_t wakeup_ack = {
    .left_obj = NULL,
    .left_label = NULL,
    .right_obj = NULL,
    .right_label = NULL,
    .ack_topic = "SYS MESSAGE",
    .notification = "正在初始化...",
    .needs_refresh = false,
    .timeout_target_tick = 0,
    .timeout_tick = 0
};

/* 统一 240x240 根背景 */
static void setup_screen_bg(lv_obj_t *screen)
{
    if (!screen) return;

    /* 不 remove_style_all，保留主题基础能力 */
    lv_obj_set_style_bg_color(screen, lv_color_hex(PAGE_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}

/* 统一 200x200 圆形面板 */
static void setup_circle_panel(lv_obj_t *obj)
{
    if (!obj) return;

    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, 200, 200);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(PANEL_BORDER_COLOR), 0);
    lv_obj_set_style_clip_corner(obj, true, 0);
    lv_obj_set_style_pad_all(obj, 12, 0);
    lv_obj_align(obj, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

/* 内部接口：设置ACK主题（用于左屏） */
static void wakeup_ack_set_topic(const char* topic)
{
    if (topic == NULL || s_wakeup_ack_mutex == NULL) return;

    if (xSemaphoreTake(s_wakeup_ack_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(wakeup_ack.ack_topic, topic, MAX_ACK_TOPIC_LEN - 1);
        wakeup_ack.ack_topic[MAX_ACK_TOPIC_LEN - 1] = '\0';
        wakeup_ack.needs_refresh = true;
        xSemaphoreGive(s_wakeup_ack_mutex);
    }
}

/* 内部接口：添加通知消息（用于右屏） */
static void wakeup_ack_add_notification(const char* notification)
{
    if (notification == NULL || s_wakeup_ack_mutex == NULL) return;

    if (xSemaphoreTake(s_wakeup_ack_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(wakeup_ack.notification, notification, MAX_NOTIFICATION_LEN - 1);
        wakeup_ack.notification[MAX_NOTIFICATION_LEN - 1] = '\0';
        wakeup_ack.needs_refresh = true;
        xSemaphoreGive(s_wakeup_ack_mutex);
    }
}

/* 整合接口：设置主题 + 通知 + 自动退出超时（ticks）。超时由本模块自行计时。 */
void screen_show_wakeup_ack(const char* topic, const char* notification, int timeout_ticks)
{
    wakeup_ack_set_topic(topic);
    wakeup_ack_add_notification(notification);
    wakeup_ack.timeout_target_tick = (timeout_ticks > 0) ? timeout_ticks : 0;
    app_ui_logic_request_screen(SCREEN_WAKEUP_ACK);
}

/* 创建左屏：圆形容器 + ACK主题 */
static void create_left_screen(lv_obj_t *parent)
{
    if (!parent) return;

    wakeup_ack.left_obj = lv_obj_create(parent);
    setup_circle_panel(wakeup_ack.left_obj);

    /* 左屏文本放到 panel 内，层级统一 */
    wakeup_ack.left_label = lv_label_create(wakeup_ack.left_obj);
    lv_label_set_text(wakeup_ack.left_label, wakeup_ack.ack_topic);
    lv_obj_set_style_text_color(wakeup_ack.left_label, lv_color_hex(TEXT_PRIMARY_COLOR), 0);
    lv_obj_set_style_text_font(wakeup_ack.left_label, &lv_font_notosans_cs_medium_14, 0);
    lv_obj_set_style_text_align(wakeup_ack.left_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(wakeup_ack.left_label, 150);
    lv_label_set_long_mode(wakeup_ack.left_label, LV_LABEL_LONG_WRAP);
    lv_obj_center(wakeup_ack.left_label);
}

/* 创建右屏：圆形容器 + 通知信息 */
static void create_right_screen(lv_obj_t *parent)
{
    if (!parent) return;

    wakeup_ack.right_obj = lv_obj_create(parent);
    setup_circle_panel(wakeup_ack.right_obj);

    wakeup_ack.right_label = lv_label_create(wakeup_ack.right_obj);
    lv_label_set_text(wakeup_ack.right_label, wakeup_ack.notification);
    lv_obj_set_style_text_color(wakeup_ack.right_label, lv_color_hex(TEXT_PRIMARY_COLOR), 0);
    lv_obj_set_style_text_font(wakeup_ack.right_label, &lv_font_notosans_cs_medium_14, 0);
    lv_obj_set_style_text_align(wakeup_ack.right_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(wakeup_ack.right_label, 150);
    lv_label_set_long_mode(wakeup_ack.right_label, LV_LABEL_LONG_WRAP);
    lv_obj_center(wakeup_ack.right_label);
}

/* 更新显示内容 */
static void update_display_content(void)
{
    if (s_wakeup_ack_mutex == NULL) return;

    /* 快速路径：无需刷新时不拿锁 */
    if (!wakeup_ack.needs_refresh) return;

    /* 拿锁，拷贝到局部缓冲区，立即释放锁 */
    char local_topic[MAX_ACK_TOPIC_LEN];
    char local_notification[MAX_NOTIFICATION_LEN];
    bool do_refresh = false;

    if (xSemaphoreTake(s_wakeup_ack_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (wakeup_ack.needs_refresh) {
            memcpy(local_topic, wakeup_ack.ack_topic, MAX_ACK_TOPIC_LEN);
            memcpy(local_notification, wakeup_ack.notification, MAX_NOTIFICATION_LEN);
            wakeup_ack.needs_refresh = false;
            do_refresh = true;
        }
        xSemaphoreGive(s_wakeup_ack_mutex);
    }

    /* 锁外操作 LVGL（LVGL 只在 UI 线程调用，无需二次加锁） */
    if (do_refresh) {
        if (wakeup_ack.left_label) {
            lv_label_set_text(wakeup_ack.left_label, local_topic);
        }
        if (wakeup_ack.right_label) {
            lv_label_set_text(wakeup_ack.right_label, local_notification);
        }
    }
}

/* 加载屏幕 */
static int wakeup_ack_load(void)
{
    static bool first_load = true;
    if(first_load)
    {
        first_load = false;
        if (s_wakeup_ack_mutex == NULL) {
            s_wakeup_ack_mutex = xSemaphoreCreateMutex();
        }
    }
    lv_obj_t *screen_left = screen_manager.screens[DISPLAY_LEFT].root;
    lv_obj_t *screen_right = screen_manager.screens[DISPLAY_RIGHT].root;

    if (!screen_left || !screen_right) {
        return -1;
    }

    /* 统一背景 */
    setup_screen_bg(screen_left);
    setup_screen_bg(screen_right);

    /* 创建左右屏内容 */
    create_left_screen(screen_left);
    create_right_screen(screen_right);

    /* 进入页面时重置本地超时计数；未显式设置则使用默认超时（约 1.5s） */
    wakeup_ack.timeout_tick = 0;
    if (wakeup_ack.timeout_target_tick <= 0) {
        wakeup_ack.timeout_target_tick = APP_UI_MS_TO_TICKS(1500);
    }

    return 0;
}

/* 更新屏幕 */
static int wakeup_ack_update(void)
{
    update_display_content();

    /* 本地超时管理：计时达到目标后自行退回到进入本页之前的页面 */
    if (wakeup_ack.timeout_target_tick > 0) {
        wakeup_ack.timeout_tick++;
        if (wakeup_ack.timeout_tick >= wakeup_ack.timeout_target_tick) {
            wakeup_ack.timeout_tick = 0;
            wakeup_ack.timeout_target_tick = 0;
            app_ui_logic_request_screen(screen_manager.old_screen);
            return 0;
        }
    }

    static uint16_t tick_counter = 0;
    if (tick_counter++ >= APP_UI_MS_TO_TICKS(500)) {
        tick_counter = 0;
        lv_timer_handler();
    }

    return 0;
}

/* 卸载屏幕 */
static int wakeup_ack_unload(void)
{
    if (wakeup_ack.left_obj) {
        lv_obj_del(wakeup_ack.left_obj);
        wakeup_ack.left_obj = NULL;
    }

    if (wakeup_ack.right_obj) {
        lv_obj_del(wakeup_ack.right_obj);
        wakeup_ack.right_obj = NULL;
    }

    /* 子对象会随父对象一起删，这里只清指针 */
    wakeup_ack.left_label = NULL;
    wakeup_ack.right_label = NULL;

    return 0;
}

/* wakeup_ack 按键逻辑已迁移至 logic_page_wakeup_ack.c */
/* 屏幕接口 */
screen_interface_t wakeup_ack_screen_interface = {
    .load = wakeup_ack_load,
    .update = wakeup_ack_update,
    .unload = wakeup_ack_unload,
    .on_key_event = logic_page_wakeup_ack_on_key,
};