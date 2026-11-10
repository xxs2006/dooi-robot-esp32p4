#include <string.h>
#include "esp_log.h"
#include <lvgl.h>
#include "app_ui.h"

static const char *TAG = "music_page";

/* -------------------- GIF Binary -------------------- */
extern const uint8_t music_pet[]     asm("_binary_music_pet_gif_start");
extern const uint8_t music_pet_end[] asm("_binary_music_pet_gif_end");

/* 左右两屏分辨率一致，GIF 全屏铺满 */
#define MUSIC_GIF_SIZE  240

/* -------------------- GIF Descriptor -------------------- */
static lv_image_dsc_t music_pet_dsc;

/* -------------------- Page Data -------------------- */
typedef struct {
    lv_obj_t *gif_left;
    lv_obj_t *gif_right;
    bool      unloading;
} music_page_data_t;

static music_page_data_t music_page = {0};

/* -------------------- Init GIF Descriptor -------------------- */
static void init_gif_desc(void)
{
    if(music_pet_dsc.data != NULL) {
        return;  /* 二进制数据在 Flash 中固定不变，只需初始化一次 */
    }

    memset(&music_pet_dsc, 0, sizeof(lv_image_dsc_t));

    music_pet_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    music_pet_dsc.header.cf    = LV_COLOR_FORMAT_RAW;
    music_pet_dsc.data         = music_pet;
    music_pet_dsc.data_size    = (uint32_t)(music_pet_end - music_pet);

    ESP_LOGI(TAG, "Music Pet GIF size: %u bytes", (unsigned)music_pet_dsc.data_size);
}

/* -------------------- Create GIF -------------------- */
/* 在指定屏幕根节点上创建一个全屏 GIF 实例。
 * 左右两屏各自持有独立的 lv_gif 对象与解码上下文，互不干扰。 */
static lv_obj_t *create_music_gif(lv_obj_t *parent)
{
    if(parent == NULL) {
        ESP_LOGE(TAG, "GIF parent is NULL");
        return NULL;
    }

    lv_obj_t *gif = lv_gif_create(parent);
    if(gif == NULL) {
        ESP_LOGE(TAG, "lv_gif_create failed");
        return NULL;
    }

    lv_gif_set_color_format(gif, LV_COLOR_FORMAT_RGB565);
    lv_gif_set_src(gif, &music_pet_dsc);

    lv_obj_set_size(gif, MUSIC_GIF_SIZE, MUSIC_GIF_SIZE);
    lv_obj_center(gif);

    return gif;
}

/* -------------------- Load -------------------- */
static int music_page_load(void)
{
    ESP_LOGI(TAG, "Loading music page...");

    lv_obj_t *root_left  = screen_manager.screens[DISPLAY_LEFT].root;
    lv_obj_t *root_right = screen_manager.screens[DISPLAY_RIGHT].root;

    if(root_left == NULL || root_right == NULL) {
        ESP_LOGE(TAG, "Screen root NULL: left=%p right=%p", root_left, root_right);
        return -1;
    }

    memset(&music_page, 0, sizeof(music_page));
    init_gif_desc();

    music_page.unloading = false;

    /* 左右两屏显示同一个 GIF */
    music_page.gif_left  = create_music_gif(root_left);
    music_page.gif_right = create_music_gif(root_right);

    if(music_page.gif_left == NULL && music_page.gif_right == NULL) {
        ESP_LOGE(TAG, "Failed to create GIF on both screens");
        return -1;
    }

    return 0;
}

/* -------------------- Update -------------------- */
static int music_page_update(void)
{
    lv_timer_handler();
    return 0;
}

/* -------------------- Unload -------------------- */
static int music_page_unload(void)
{
    ESP_LOGI(TAG, "Unloading music page...");

    music_page.unloading = true;

    if(music_page.gif_left) {
        lv_obj_del_async(music_page.gif_left);
        music_page.gif_left = NULL;
    }

    if(music_page.gif_right) {
        lv_obj_del_async(music_page.gif_right);
        music_page.gif_right = NULL;
    }

    return 0;
}

/* -------------------- Interface Export -------------------- */
screen_interface_t music_screen_interface = {
    .load         = music_page_load,
    .update       = music_page_update,
    .unload       = music_page_unload,
    .on_key_event = logic_page_music_on_key,
};
