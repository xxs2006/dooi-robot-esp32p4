/**
 * @file  custom_page.c
 * @brief 二维码页面：左右两屏各显示 manual.png，20 s 后自动切回上一屏。
 *        任意按键也可立即返回。
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "esp_log.h"
#include "lvgl.h"
#include "app_ui.h"

static const char *TAG = "custom_page";

/* ------------------------------------------------------------------ */
/*  嵌入式图片二进制（CMakeLists 中需用 EMBED_FILES 包含 manual.png）  */
/* ------------------------------------------------------------------ */
extern const uint8_t manual_png_start[] asm("_binary_manual_png_start");
extern const uint8_t manual_png_end[]   asm("_binary_manual_png_end");

/* ------------------------------------------------------------------ */
/*  超时配置                                                           */
/* ------------------------------------------------------------------ */
#define CUSTOM_PAGE_TIMEOUT_MS    30000u
#define CUSTOM_PAGE_TIMEOUT_TICKS APP_UI_MS_TO_TICKS(CUSTOM_PAGE_TIMEOUT_MS)  /* ≈ 606 ticks */

/* ------------------------------------------------------------------ */
/*  页面私有数据                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    lv_image_dsc_t qr_dsc;        /* PNG 图片描述符，左右共用同一份数据   */
    lv_obj_t      *left_img;      /* 左屏图片对象                        */
    lv_obj_t      *right_img;     /* 右屏图片对象                        */
    uint16_t       timeout_tick;  /* 已累计的 tick 数                    */
    bool           unloading;     /* 正在卸载，防止重入                   */
} custom_page_data_t;

static custom_page_data_t g_page = {0};

/* ------------------------------------------------------------------ */
/*  内部工具                                                            */
/* ------------------------------------------------------------------ */

/** 返回进入本页前的屏幕，找不到时兜底为数字时钟 */
static void go_back(void)
{
    screen_type_t prev = screen_manager.old_screen;
    if (prev == SCREEN_NULL || prev == SCREEN_WAKEUP_ACK) {
        prev = SCREEN_EYES;
    }
    screen_set_current(prev);
}

/** 初始化图片描述符（PNG 需要 LVGL PNG 解码器支持，与 GIF 同理） */
static void init_qr_dsc(void)
{
    memset(&g_page.qr_dsc, 0, sizeof(lv_image_dsc_t));
    g_page.qr_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    g_page.qr_dsc.header.cf   = LV_COLOR_FORMAT_RAW;   /* 告知 LVGL 需要解码 */
    g_page.qr_dsc.data         = manual_png_start;
    g_page.qr_dsc.data_size    = (uint32_t)(manual_png_end - manual_png_start);

    ESP_LOGI(TAG, "manual.png: %u bytes", g_page.qr_dsc.data_size);
}

/** 在指定父容器中创建居中的二维码图片 */
static lv_obj_t *create_qr_img(lv_obj_t *parent)
{
    if (parent == NULL) return NULL;

    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, &g_page.qr_dsc);

    /* PNG 原图若为 200×200，则保持原始尺寸 */
    lv_image_set_scale(img, 256);

    /* 居中到 240×240 屏幕，四边留白 */
    lv_obj_center(img);

    return img;
}

/* ================================================================== */
/*  Screen Interface 实现                                              */
/* ================================================================== */

/* ---------------------------- load -------------------------------- */
static int custom_page_load(void)
{
    ESP_LOGI(TAG, "Loading custom_page ...");

    lv_obj_t *root_l = screen_manager.screens[DISPLAY_LEFT].root;
    lv_obj_t *root_r = screen_manager.screens[DISPLAY_RIGHT].root;

    if (root_l == NULL || root_r == NULL) {
        ESP_LOGE(TAG, "Screen root NULL: left=%p right=%p", root_l, root_r);
        return -1;
    }

    memset(&g_page, 0, sizeof(g_page));
    init_qr_dsc();

    /* 左右屏各创建一个相同的二维码图片 */
    g_page.left_img  = create_qr_img(root_l);
    g_page.right_img = create_qr_img(root_r);

    g_page.timeout_tick = 0;
    g_page.unloading    = false;

    ESP_LOGI(TAG, "custom_page loaded — auto-return in %u s",
             CUSTOM_PAGE_TIMEOUT_MS / 1000u);
    return 0;
}

/* --------------------------- update ------------------------------- */
static int custom_page_update(void)
{
    if (g_page.unloading) return 0;

    /* 每次 update 被调用相当于走过 APP_UI_TICK_MS(33 ms) */
    g_page.timeout_tick++;

    if (g_page.timeout_tick >= CUSTOM_PAGE_TIMEOUT_TICKS) {
        ESP_LOGI(TAG, "timeout — returning to previous screen.");
        go_back();   /* 内部调用 screen_set_current，会触发 unload */
        return 0;
    }

    lv_timer_handler();
    return 0;
}

/* --------------------------- unload ------------------------------- */
static int custom_page_unload(void)
{
    ESP_LOGI(TAG, "Unloading custom_page ...");
    g_page.unloading = true;

    if (g_page.left_img) {
        lv_obj_del_async(g_page.left_img);
        g_page.left_img = NULL;
    }
    if (g_page.right_img) {
        lv_obj_del_async(g_page.right_img);
        g_page.right_img = NULL;
    }

    return 0;
}

/* ----------------------- on_key_event ----------------------------- */
void custom_page_go_back(void)
{
    go_back();
}

/* ================================================================== */
/*  对外导出接口                                                        */
/* ================================================================== */
screen_interface_t custom_page_screen_interface = {
    .load         = custom_page_load,
    .update       = custom_page_update,
    .unload       = custom_page_unload,
    .on_key_event = logic_page_custom_on_key,
};