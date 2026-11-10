#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "esp_log.h"
#include <lvgl.h>
#include "app_ui.h"
#include "esp_timer.h" 

static const char *TAG = "lucky_cat";

/*
 * 右侧舵机周期摆动参数
 * 修改这些宏即可调整摆动效果
 */
#define SERVO_SWING_MIN_ANGLE    30.0f   /* 最小角度（前摆极限）*/
#define SERVO_SWING_MAX_ANGLE    90.0f   /* 最大角度（指向正上方，停止点）*/
#define SERVO_SWING_STEP_MS      20      /* 每步时间间隔 (ms) */
#define SERVO_SWING_STEP_ANGLE   2.0f    /* 每步角度增量 (°)，越大越快 */

/* -------------------- Servo State -------------------- */
typedef struct {
    uint32_t last_step_ms;
    bool     going_down;     /* true = 向 30° 摆，false = 向 90° 摆 */
    float    current_angle;
} servo_swing_state_t;

static servo_swing_state_t s_swing = {
    .last_step_ms  = 0,
    .going_down    = true,
    .current_angle = 90.0f,
};

/* -------------------- Persistent Initial Style -------------------- */
static int s_initial_style_index = 0;  /* 跨 load/unload 保持 */


/* -------------------- GIF Binary -------------------- */
extern const uint8_t lucky_cat_red[] asm("_binary_lucky_cat_red_gif_start");
extern const uint8_t lucky_cat_red_end[] asm("_binary_lucky_cat_red_gif_end");

extern const uint8_t lucky_cat_yellow[] asm("_binary_lucky_cat_yellow_gif_start");
extern const uint8_t lucky_cat_yellow_end[] asm("_binary_lucky_cat_yellow_gif_end");

extern const uint8_t lucky_cat_purple[] asm("_binary_lucky_cat_purple_gif_start");
extern const uint8_t lucky_cat_purple_end[] asm("_binary_lucky_cat_purple_gif_end");

extern const uint8_t lucky_cat_blue[] asm("_binary_lucky_cat_blue_gif_start");
extern const uint8_t lucky_cat_blue_end[] asm("_binary_lucky_cat_blue_gif_end");

/* -------------------- FONT -------------------- */
LV_FONT_DECLARE(lv_font_notosans_cs_medium_14);

/* -------------------- GIF Descriptor -------------------- */
static lv_image_dsc_t lucky_cat_red_dsc;
static lv_image_dsc_t lucky_cat_yellow_dsc;
static lv_image_dsc_t lucky_cat_purple_dsc;
static lv_image_dsc_t lucky_cat_blue_dsc;

/* -------------------- Style Struct -------------------- */
typedef struct {
    const char *name;
    lv_image_dsc_t *dsc;
    uint32_t color;  /* 对应的主题色 */
} gif_style_t;

/* -------------------- Style List -------------------- */
static gif_style_t gif_styles[] = {
    {"招财猫 \n RED TYPE", &lucky_cat_red_dsc, 0xFF0000},
    {"招财猫 \n YELLOW TYPE", &lucky_cat_yellow_dsc, 0xFFD700},
    {"招财猫 \n PURPLE TYPE", &lucky_cat_purple_dsc, 0x9370DB},
    {"招财猫 \n BLUE TYPE", &lucky_cat_blue_dsc, 0x1E90FF},
};

#define STYLE_COUNT (sizeof(gif_styles) / sizeof(gif_style_t))

/* -------------------- Page Data -------------------- */
typedef struct {
    lv_obj_t *left_container;
    lv_obj_t *left_title_label;
    lv_obj_t *left_style_label;
    lv_obj_t *left_index_label;
    lv_obj_t *left_hint_label;

    lv_obj_t *right_parent;
    lv_obj_t *gif_obj;

    int current_style_index;
    int pending_style_index;
    bool switch_requested;
    bool unloading;
} lucky_cat_data_t;

static lucky_cat_data_t lucky_cat = {0};

/* -------------------- Init GIF Descriptor -------------------- */
static void init_gif_desc(void)
{
    memset(&lucky_cat_red_dsc, 0, sizeof(lv_image_dsc_t));
    memset(&lucky_cat_yellow_dsc, 0, sizeof(lv_image_dsc_t));
    memset(&lucky_cat_purple_dsc, 0, sizeof(lv_image_dsc_t));
    memset(&lucky_cat_blue_dsc, 0, sizeof(lv_image_dsc_t));

    lucky_cat_red_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    lucky_cat_red_dsc.header.cf = LV_COLOR_FORMAT_RAW;
    lucky_cat_red_dsc.data = lucky_cat_red;
    lucky_cat_red_dsc.data_size = lucky_cat_red_end - lucky_cat_red;

    lucky_cat_yellow_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    lucky_cat_yellow_dsc.header.cf = LV_COLOR_FORMAT_RAW;
    lucky_cat_yellow_dsc.data = lucky_cat_yellow;
    lucky_cat_yellow_dsc.data_size = lucky_cat_yellow_end - lucky_cat_yellow;

    lucky_cat_purple_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    lucky_cat_purple_dsc.header.cf = LV_COLOR_FORMAT_RAW;
    lucky_cat_purple_dsc.data = lucky_cat_purple;
    lucky_cat_purple_dsc.data_size = lucky_cat_purple_end - lucky_cat_purple;

    lucky_cat_blue_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    lucky_cat_blue_dsc.header.cf = LV_COLOR_FORMAT_RAW;
    lucky_cat_blue_dsc.data = lucky_cat_blue;
    lucky_cat_blue_dsc.data_size = lucky_cat_blue_end - lucky_cat_blue;
}

/* -------------------- Update Left Info -------------------- */
static void update_left_info(void)
{
    if(lucky_cat.current_style_index < 0 || lucky_cat.current_style_index >= STYLE_COUNT) {
        return;
    }

    const gif_style_t *style = &gif_styles[lucky_cat.current_style_index];

    if(lucky_cat.left_style_label) {
        lv_label_set_text(lucky_cat.left_style_label, style->name);
        lv_obj_set_style_text_color(lucky_cat.left_style_label, lv_color_hex(style->color), 0);
    }

    if(lucky_cat.left_container) {
        lv_obj_set_style_border_color(lucky_cat.left_container, lv_color_hex(style->color), 0);}

    if(lucky_cat.left_index_label) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d/%d",
                 lucky_cat.current_style_index + 1,
                 STYLE_COUNT);
        lv_label_set_text(lucky_cat.left_index_label, buf);
    }
}

/* -------------------- Really Switch GIF -------------------- */
static void switch_gif_style_internal(int new_index)
{
    if(lucky_cat.unloading) {
        return;
    }

    if(new_index < 0 || new_index >= STYLE_COUNT) {
        ESP_LOGW(TAG, "Invalid style index: %d", new_index);
        return;
    }

    if(lucky_cat.current_style_index == new_index && lucky_cat.gif_obj != NULL) {
        return;
    }

    const gif_style_t *style = &gif_styles[new_index];
    ESP_LOGI(TAG, "Switch GIF: %s", style->name);

    /* 先异步删除旧对象，避免渲染过程中直接删 */
    if(lucky_cat.gif_obj) {
        lv_obj_del_async(lucky_cat.gif_obj);
        lucky_cat.gif_obj = NULL;
    }

    if(lucky_cat.right_parent == NULL) {
        ESP_LOGE(TAG, "right_parent is NULL");
        return;
    }

    lucky_cat.gif_obj = lv_gif_create(lucky_cat.right_parent);
    if(lucky_cat.gif_obj == NULL) {
        ESP_LOGE(TAG, "lv_gif_create failed");
        return;
    }

    /* 根据你的屏幕格式选，常见是 RGB565 */
    lv_gif_set_color_format(lucky_cat.gif_obj, LV_COLOR_FORMAT_RGB565);
    lv_gif_set_src(lucky_cat.gif_obj, style->dsc);

    lv_obj_set_size(lucky_cat.gif_obj, 240, 240);
    lv_obj_center(lucky_cat.gif_obj);

    lucky_cat.current_style_index = new_index;
    update_left_info();
}

/* -------------------- Request Switch -------------------- */
static void request_switch_gif_style(int new_index)
{
    if(new_index < 0 || new_index >= STYLE_COUNT) {
        return;
    }

    lucky_cat.pending_style_index = new_index;
    lucky_cat.switch_requested = true;
}

/* -------------------- Left Screen -------------------- */
static void create_left_screen(lv_obj_t *parent)
{
    if(parent == NULL) {
        ESP_LOGE(TAG, "Left parent is NULL");
        return;
    }

    lucky_cat.left_container = lv_obj_create(parent);
    lv_obj_remove_style_all(lucky_cat.left_container);
    lv_obj_set_size(lucky_cat.left_container, 200, 200);
    lv_obj_set_style_radius(lucky_cat.left_container, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(lucky_cat.left_container, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lucky_cat.left_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lucky_cat.left_container, 2, 0);
    lv_obj_center(lucky_cat.left_container);
    lv_obj_clear_flag(lucky_cat.left_container, LV_OBJ_FLAG_SCROLLABLE);

    lucky_cat.left_title_label = lv_label_create(lucky_cat.left_container);
    lv_label_set_text(lucky_cat.left_title_label, "LUCKY CAT");
    lv_obj_set_style_text_color(lucky_cat.left_title_label, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_font(lucky_cat.left_title_label, &lv_font_montserrat_14, 0);
    lv_obj_align(lucky_cat.left_title_label, LV_ALIGN_CENTER, 0, -70);

    lucky_cat.left_style_label = lv_label_create(lucky_cat.left_container);
    lv_label_set_text(lucky_cat.left_style_label, "");
    lv_obj_set_style_text_font(lucky_cat.left_style_label, &lv_font_notosans_cs_medium_14, 0);
    lv_obj_set_width(lucky_cat.left_style_label, 160);
    lv_obj_set_style_text_align(lucky_cat.left_style_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lucky_cat.left_style_label, LV_ALIGN_CENTER, 0, -20);

    lucky_cat.left_index_label = lv_label_create(lucky_cat.left_container);
    lv_label_set_text(lucky_cat.left_index_label, "1/4");
    lv_obj_set_style_text_color(lucky_cat.left_index_label, lv_color_hex(0xA0A0A0), 0);
    lv_obj_set_style_text_font(lucky_cat.left_index_label, &lv_font_montserrat_14, 0);
    lv_obj_align(lucky_cat.left_index_label, LV_ALIGN_CENTER, 0, 15);

    lucky_cat.left_hint_label = lv_label_create(lucky_cat.left_container);
    lv_label_set_text(lucky_cat.left_hint_label, "L:Prev R:Next\nDouble:Back");
    lv_obj_set_style_text_color(lucky_cat.left_hint_label, lv_color_hex(0x606060), 0);
    lv_obj_set_style_text_font(lucky_cat.left_hint_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(lucky_cat.left_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lucky_cat.left_hint_label, 160);
    lv_obj_align(lucky_cat.left_hint_label, LV_ALIGN_CENTER, 0, 60);
}

/* -------------------- Key Event 动作接口（供 logic_page_lucky_cat 调用） -------------------- */
void lucky_cat_style_switch(int delta)
{
    int cur = lucky_cat.current_style_index;
    int next = cur + delta;
    if (next < 0)             next = STYLE_COUNT - 1;
    if (next >= STYLE_COUNT)  next = 0;
    request_switch_gif_style(next);
}

/* -------------------- Load -------------------- */
static int lucky_cat_load(void)
{
    ESP_LOGI(TAG, "Loading lucky cat page...");

    lv_obj_t *screen_left = screen_manager.screens[DISPLAY_LEFT].root;
    lv_obj_t *screen_right = screen_manager.screens[DISPLAY_RIGHT].root;

    if(screen_left == NULL || screen_right == NULL) {
        ESP_LOGE(TAG, "Screen root NULL: left=%p right=%p", screen_left, screen_right);
        return -1;
    }

    memset(&lucky_cat, 0, sizeof(lucky_cat));
    init_gif_desc();

    lucky_cat.current_style_index = -1;
    lucky_cat.pending_style_index = 0;
    lucky_cat.switch_requested = false;
    lucky_cat.unloading = false;
    lucky_cat.right_parent = screen_right;

    create_left_screen(screen_left);

    /* 使用持久变量作为初始风格，而非硬编码 0 */
    request_switch_gif_style(s_initial_style_index);

    return 0;
}

/* -------------------- Update -------------------- */
static int lucky_cat_update(void)
{
    /* ---- 处理 GIF 切换请求 ---- */
    if (lucky_cat.switch_requested) {
        lucky_cat.switch_requested = false;
        switch_gif_style_internal(lucky_cat.pending_style_index);
    }

    /* ---- 右侧舵机摆动 (30°~90°) ---- */
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    if ((now_ms - s_swing.last_step_ms) >= SERVO_SWING_STEP_MS) {
        s_swing.last_step_ms = now_ms;

        if (s_swing.going_down) {
            s_swing.current_angle -= SERVO_SWING_STEP_ANGLE;
            if (s_swing.current_angle <= SERVO_SWING_MIN_ANGLE) {
                s_swing.current_angle = SERVO_SWING_MIN_ANGLE;
                s_swing.going_down = false;
            }
        } else {
            s_swing.current_angle += SERVO_SWING_STEP_ANGLE;
            if (s_swing.current_angle >= SERVO_SWING_MAX_ANGLE) {
                s_swing.current_angle = SERVO_SWING_MAX_ANGLE;
                s_swing.going_down = true;
            }
        }

        /* duration_ms = 0：直接让驱动的插值处理平滑，无需额外时间参数 */
        app_servo_set_angle(SERVO_RIGHT, s_swing.current_angle, 0);
    }

    lv_timer_handler();
    return 0;
}

/* -------------------- Unload -------------------- */
static int lucky_cat_unload(void)
{
    ESP_LOGI(TAG, "Unloading lucky cat page...");

    lucky_cat.unloading = true;

    if(lucky_cat.gif_obj) {
        lv_obj_del_async(lucky_cat.gif_obj);
        lucky_cat.gif_obj = NULL;
    }

    if(lucky_cat.left_container) {
        lv_obj_del_async(lucky_cat.left_container);
        lucky_cat.left_container = NULL;
    }

    lucky_cat.right_parent = NULL;
    lucky_cat.left_title_label = NULL;
    lucky_cat.left_style_label = NULL;
    lucky_cat.left_index_label = NULL;
    lucky_cat.left_hint_label = NULL;

    app_servo_set_target_angle(SERVO_TARGET_BOTH, 135, 1500);

    return 0;
}

/**
 * @brief 设置招财猫动画风格
 * @param style 1-based 风格编号（1=红色招财猫, 2=黄色招财猫, 3=紫色招财猫, 4=蓝色招财猫）
 *              超出范围则 clamp 到有效区间
 * @note  若页面已加载则立即切换，否则记录下来等 load 时使用
 */
void lucky_cat_style_set(int style)
{
    /* MCP 传入 1-based，内部使用 0-based */
    int index = style - 1;

    /* clamp 到有效范围 */
    if (index < 0) {
        index = 0;
    } else if (index >= (int)STYLE_COUNT) {
        index = (int)STYLE_COUNT - 1;
    }

    ESP_LOGI(TAG, "Set style: %d (index=%d/%d)", style, index, (int)STYLE_COUNT);

    /* 保存到持久变量，供下次 load 使用 */
    s_initial_style_index = index;

    /* 如果页面已经加载，立即请求切换 */
    if (!lucky_cat.unloading && lucky_cat.right_parent != NULL) {
        request_switch_gif_style(index);
    }
}

/* -------------------- Interface -------------------- */
screen_interface_t lucky_cat_screen_interface = {
    .load = lucky_cat_load,
    .update = lucky_cat_update,
    .unload = lucky_cat_unload,
    .on_key_event = logic_page_lucky_cat_on_key,
};