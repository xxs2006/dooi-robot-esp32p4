/**
 * @file screen_weather.c
 * @brief 天气显示屏幕
 *
 * 左屏：天气图片 + 地点名称
 * 右屏：天气状况文字 + 温度（带颜色映射）
 *
 * 数据通过 weather_screen_set_data() 注入，线程安全。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#include "app_ui.h"

static const char *TAG = "screen_weather";

/* ------------------------------------------------------------------ */
/*  嵌入式图片二进制（CMakeLists 中需用 EMBED_FILES 包含天气图标）          */
/* ------------------------------------------------------------------ */
extern const uint8_t sunny_png_start[]        asm("_binary_sunny_png_start");
extern const uint8_t sunny_png_end[]          asm("_binary_sunny_png_end");
extern const uint8_t partly_cloudy_png_start[] asm("_binary_partly_cloudy_png_start");
extern const uint8_t partly_cloudy_png_end[]   asm("_binary_partly_cloudy_png_end");
extern const uint8_t cloudy_png_start[]       asm("_binary_cloudy_png_start");
extern const uint8_t cloudy_png_end[]         asm("_binary_cloudy_png_end");
extern const uint8_t foggy_png_start[]        asm("_binary_foggy_png_start");
extern const uint8_t foggy_png_end[]          asm("_binary_foggy_png_end");
extern const uint8_t drizzle_png_start[]      asm("_binary_drizzle_png_start");
extern const uint8_t drizzle_png_end[]        asm("_binary_drizzle_png_end");
extern const uint8_t rainy_png_start[]        asm("_binary_rainy_png_start");
extern const uint8_t rainy_png_end[]          asm("_binary_rainy_png_end");
extern const uint8_t heavy_rain_png_start[]   asm("_binary_heavy_rain_png_start");
extern const uint8_t heavy_rain_png_end[]     asm("_binary_heavy_rain_png_end");
extern const uint8_t snowy_png_start[]        asm("_binary_snowy_png_start");
extern const uint8_t snowy_png_end[]          asm("_binary_snowy_png_end");
extern const uint8_t heavy_snow_png_start[]   asm("_binary_heavy_snow_png_start");
extern const uint8_t heavy_snow_png_end[]     asm("_binary_heavy_snow_png_end");

/* ------------------------------------------------------------------ */
/*  字体声明                                                             */
/* ------------------------------------------------------------------ */
LV_FONT_DECLARE(lv_font_notosans_cs_medium_14);

/* ------------------------------------------------------------------ */
/*  布局常量                                                             */
/* ------------------------------------------------------------------ */
#define SCREEN_W              240
#define SCREEN_H              240

#define BG_COLOR              0x0A0F1E   /* 深蓝黑背景，营造天空感 */
#define PANEL_BORDER_COLOR    0x3B82F6   /* 蓝色边框，与现有风格一致 */

#define COLOR_HOT             0xFF5733   /* 高温红 */
#define COLOR_WARM            0xFFA500   /* 温暖橙 */
#define COLOR_NORMAL          0xFFFFFF   /* 常温白 */
#define COLOR_COOL            0x87CEEB   /* 凉爽天蓝 */
#define COLOR_COLD            0x7EC8E3   /* 寒冷浅蓝 */
#define COLOR_FREEZE          0xADD8E6   /* 冰冻冰蓝 */

#define MAX_LOCATION_LEN      32
#define MAX_CONDITION_LEN     32

/* ------------------------------------------------------------------ */
/*  天气图标映射表                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *condition;
    const uint8_t *png_start;
    const uint8_t *png_end;
} weather_icon_map_t;

static const weather_icon_map_t weather_icon_map[] = {
    { "晴天",    sunny_png_start,        sunny_png_end        },
    { "多云",    partly_cloudy_png_start, partly_cloudy_png_end },
    { "阴天",    cloudy_png_start,       cloudy_png_end       },
    { "雾/霾",   foggy_png_start,        foggy_png_end        },
    { "小雨",    drizzle_png_start,      drizzle_png_end      },
    { "中雨",    rainy_png_start,        rainy_png_end        },
    { "暴雨",    heavy_rain_png_start,   heavy_rain_png_end   },
    { "小雪",    snowy_png_start,        snowy_png_end        },
    { "大雪",    heavy_snow_png_start,   heavy_snow_png_end   },
};

#define WEATHER_ICON_MAP_SIZE (sizeof(weather_icon_map) / sizeof(weather_icon_map_t))

static const lv_image_dsc_t *get_weather_icon(const char *condition)
{
    static lv_image_dsc_t img_dsc;
    memset(&img_dsc, 0, sizeof(lv_image_dsc_t));
    img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    img_dsc.header.cf   = LV_COLOR_FORMAT_RAW;

    for (int i = 0; i < WEATHER_ICON_MAP_SIZE; i++) {
        if (strcmp(condition, weather_icon_map[i].condition) == 0) {
            img_dsc.data      = weather_icon_map[i].png_start;
            img_dsc.data_size = (uint32_t)(weather_icon_map[i].png_end - weather_icon_map[i].png_start);
            ESP_LOGI(TAG, "Weather icon: %s (%u bytes)", condition, img_dsc.data_size);
            return &img_dsc;
        }
    }

    img_dsc.data      = sunny_png_start;
    img_dsc.data_size = (uint32_t)(sunny_png_end - sunny_png_start);
    ESP_LOGI(TAG, "Unknown weather '%s', use sunny.png as default", condition);
    return &img_dsc;
}

/* ------------------------------------------------------------------ */
/*  私有数据结构                                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    /* 左屏控件 */
    lv_obj_t        *left_panel;       /* 圆形面板容器 */
    lv_image_dsc_t   left_img_dsc;     /* 天气图片描述符 */
    lv_obj_t        *left_weather_img; /* 天气图片对象 */
    lv_obj_t        *left_loc_label;   /* 地点名称 */

    /* 右屏控件 */
    lv_obj_t        *right_panel;      /* 圆形面板容器 */
    lv_obj_t        *right_cond_label; /* 天气状况文字 */
    lv_obj_t        *right_temp_label; /* 温度数值 */

    /* 数据缓存（由外部线程写入，UI 线程读取） */
    char            location[MAX_LOCATION_LEN];
    char            condition_zh[MAX_CONDITION_LEN];
    int             temperature;

    bool            needs_refresh;
} weather_data_t;

static weather_data_t s_wd = {
    .left_panel       = NULL,
    .left_weather_img = NULL,
    .left_loc_label   = NULL,
    .right_panel      = NULL,
    .right_cond_label = NULL,
    .right_temp_label = NULL,
    .location         = "---",
    .condition_zh     = "---",
    .temperature      = 0,
    .needs_refresh    = false,
};

/* ------------------------------------------------------------------ */
/*  外部数据注入接口（可在任意任务中调用）                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief 更新天气页面显示数据
 *
 * @param location     地点名称，如 "深圳"
 * @param condition_zh 天气状况中文，如 "晴天"
 * @param temperature  温度（摄氏度）
 */
void weather_screen_set_data(const char *location,
                             const char *condition_zh,
                             int         temperature)
{
    if (!location || !condition_zh) return;

    strncpy(s_wd.location,     location,     MAX_LOCATION_LEN  - 1);
    strncpy(s_wd.condition_zh, condition_zh, MAX_CONDITION_LEN - 1);
    s_wd.location[MAX_LOCATION_LEN   - 1] = '\0';
    s_wd.condition_zh[MAX_CONDITION_LEN - 1] = '\0';
    s_wd.temperature   = temperature;
    s_wd.needs_refresh = true;

}

/** 更新天气图片描述符 */
static void update_weather_img_dsc(const char *condition)
{
    const lv_image_dsc_t *icon_dsc = get_weather_icon(condition);
    s_wd.left_img_dsc = *icon_dsc;
}

/* ------------------------------------------------------------------ */
/*  温度颜色映射                                                          */
/* ------------------------------------------------------------------ */
/**
 * @brief 根据温度返回对应颜色，视觉上直观感受冷暖
 */
static lv_color_t temp_to_color(int temp)
{
    if (temp >= 35)       return lv_color_hex(COLOR_HOT);
    if (temp >= 25)       return lv_color_hex(COLOR_WARM);
    if (temp >= 10)       return lv_color_hex(COLOR_NORMAL);
    if (temp >= 0)        return lv_color_hex(COLOR_COOL);
    if (temp >= -10)      return lv_color_hex(COLOR_COLD);
    return                       lv_color_hex(COLOR_FREEZE);
}

/* ------------------------------------------------------------------ */
/*  辅助：通用背景设置                                                    */
/* ------------------------------------------------------------------ */
static void setup_screen_bg(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}

/* ------------------------------------------------------------------ */
/*  辅助：通用圆形面板                                                    */
/* ------------------------------------------------------------------ */
static void setup_circle_panel(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, 210, 210);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(PANEL_BORDER_COLOR), 0);
    lv_obj_set_style_clip_corner(obj, true, 0);
    lv_obj_set_style_pad_all(obj, 8, 0);
    lv_obj_align(obj, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

/* ------------------------------------------------------------------ */
/*  创建左屏：天气图片 + 地点                                              */
/* ------------------------------------------------------------------ */
static void create_left_screen(lv_obj_t *parent)
{
    /* 圆形面板 */
    s_wd.left_panel = lv_obj_create(parent);
    setup_circle_panel(s_wd.left_panel);

    /* 天气图片 */
    s_wd.left_weather_img = lv_image_create(s_wd.left_panel);
    lv_image_set_src(s_wd.left_weather_img, &s_wd.left_img_dsc);
    lv_image_set_scale(s_wd.left_weather_img, 256);
    lv_obj_align(s_wd.left_weather_img, LV_ALIGN_CENTER, 0, -20);

    /* 地点名称（次字号） */
    s_wd.left_loc_label = lv_label_create(s_wd.left_panel);
    lv_label_set_text(s_wd.left_loc_label, s_wd.location);
    lv_obj_set_style_text_font(s_wd.left_loc_label,
                               &lv_font_notosans_cs_medium_14, 0);
    lv_obj_set_style_text_color(s_wd.left_loc_label,
                                lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_align(s_wd.left_loc_label,
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_wd.left_loc_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_wd.left_loc_label, 160);
    lv_obj_align(s_wd.left_loc_label, LV_ALIGN_CENTER, 0, 28);
}

/* ------------------------------------------------------------------ */
/*  创建右屏：天气状况 + 温度                                              */
/* ------------------------------------------------------------------ */
static void create_right_screen(lv_obj_t *parent)
{
    /* 圆形面板 */
    s_wd.right_panel = lv_obj_create(parent);
    setup_circle_panel(s_wd.right_panel);

    /* 天气状况文字 */
    s_wd.right_cond_label = lv_label_create(s_wd.right_panel);
    lv_label_set_text(s_wd.right_cond_label, s_wd.condition_zh);
    lv_obj_set_style_text_font(s_wd.right_cond_label,
                               &lv_font_notosans_cs_medium_14, 0);
    lv_obj_set_style_text_color(s_wd.right_cond_label,
                                lv_color_white(), 0);
    lv_obj_set_style_text_align(s_wd.right_cond_label,
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_wd.right_cond_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_wd.right_cond_label, 160);
    lv_obj_align(s_wd.right_cond_label, LV_ALIGN_CENTER, 0, -20);

    /* 温度数值（带颜色） */
    char temp_buf[16];
    snprintf(temp_buf, sizeof(temp_buf),
             "%s%d°C",
             (s_wd.temperature > 0 ? "+" : ""),
             s_wd.temperature);

    s_wd.right_temp_label = lv_label_create(s_wd.right_panel);
    lv_label_set_text(s_wd.right_temp_label, temp_buf);
    lv_obj_set_style_text_font(s_wd.right_temp_label,
                               &lv_font_notosans_cs_medium_14, 0);
    lv_obj_set_style_text_color(s_wd.right_temp_label,
                                temp_to_color(s_wd.temperature), 0);
    lv_obj_set_style_text_align(s_wd.right_temp_label,
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_wd.right_temp_label, LV_ALIGN_CENTER, 0, 28);
}

/* ------------------------------------------------------------------ */
/*  刷新控件内容（仅在 UI 线程调用）                                       */
/* ------------------------------------------------------------------ */
static void refresh_display(void)
{
    if (!s_wd.needs_refresh) return;

    /* 锁内拷贝，锁外操作 LVGL */
    char  loc[MAX_LOCATION_LEN];
    char  cond[MAX_CONDITION_LEN];
    int   temp;

    memcpy(loc,   s_wd.location,     MAX_LOCATION_LEN);
    memcpy(cond,  s_wd.condition_zh, MAX_CONDITION_LEN);
    temp = s_wd.temperature;
    s_wd.needs_refresh = false;


    /* 更新左屏 */
    if (s_wd.left_weather_img) {
        update_weather_img_dsc(cond);
        lv_image_set_src(s_wd.left_weather_img, &s_wd.left_img_dsc);
    }
    if (s_wd.left_loc_label) {
        lv_label_set_text(s_wd.left_loc_label, loc);
    }

    /* 更新右屏 */
    if (s_wd.right_cond_label) {
        lv_label_set_text(s_wd.right_cond_label, cond);
    }
    if (s_wd.right_temp_label) {
        char temp_buf[16];
        snprintf(temp_buf, sizeof(temp_buf),
                 "%s%d°C",
                 (temp > 0 ? "+" : ""),
                 temp);
        lv_label_set_text(s_wd.right_temp_label, temp_buf);
        lv_obj_set_style_text_color(s_wd.right_temp_label,
                                    temp_to_color(temp), 0);
    }
}

/* ------------------------------------------------------------------ */
/*  screen_interface_t 实现                                             */
/* ------------------------------------------------------------------ */

static int weather_load(void)
{
    lv_obj_t *screen_left  = screen_manager.screens[DISPLAY_LEFT].root;
    lv_obj_t *screen_right = screen_manager.screens[DISPLAY_RIGHT].root;

    if (!screen_left || !screen_right) {
        ESP_LOGE(TAG, "screen root is NULL");
        return -1;
    }

    setup_screen_bg(screen_left);
    setup_screen_bg(screen_right);

    update_weather_img_dsc(s_wd.condition_zh);

    create_left_screen(screen_left);
    create_right_screen(screen_right);

    s_wd.needs_refresh = true;

    ESP_LOGI(TAG, "weather screen loaded: %s %s %d°C",
             s_wd.location, s_wd.condition_zh, s_wd.temperature);
    return 0;
}

static int weather_update(void)
{
    static uint16_t tick_counter = 0;
    if (tick_counter++ >= APP_UI_MS_TO_TICKS(10000)) {
        tick_counter = 0;
        screen_set_current(screen_manager.old_screen);
    }
    refresh_display();
    lv_timer_handler();
    return 0;
}

static int weather_unload(void)
{
    /* 删除面板（子控件自动随父销毁） */
    if (s_wd.left_panel) {
        lv_obj_del(s_wd.left_panel);
        s_wd.left_panel       = NULL;
        s_wd.left_weather_img = NULL;
        s_wd.left_loc_label   = NULL;
    }
    if (s_wd.right_panel) {
        lv_obj_del(s_wd.right_panel);
        s_wd.right_panel      = NULL;
        s_wd.right_cond_label = NULL;
        s_wd.right_temp_label = NULL;
    }
    ESP_LOGI(TAG, "weather screen unloaded");
    return 0;
}

/* 天气页按键逻辑已迁移至 logic_page_weather.c */


/* ------------------------------------------------------------------ */
/*  对外暴露的接口结构体                                                   */
/* ------------------------------------------------------------------ */
screen_interface_t weather_screen_interface = {
    .load          = weather_load,
    .update        = weather_update,
    .unload        = weather_unload,
    .on_key_event  = logic_page_weather_on_key,
};