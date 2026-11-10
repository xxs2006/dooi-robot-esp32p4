// info_settings.c  —— 移除电池/IP，重新整理左屏布局
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include <lvgl.h>
#include "app_ui.h"
#include "app_ble_control.h"

#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "info_settings";

LV_FONT_DECLARE(lv_font_notosans_cs_medium_14);

typedef struct {
    /* 左屏 */
    lv_obj_t *left_container;
    lv_obj_t *left_title_label;
    lv_obj_t *author_label;
    lv_obj_t *version_label;
    lv_obj_t *build_label;      /* 新增：编译日期或副标题 */

    /* 右屏 */
    lv_obj_t *right_container;
    lv_obj_t *right_title_label;
    lv_obj_t *ble_item;
    lv_obj_t *ble_label;
    lv_obj_t *ble_switch;

    bool ble_on;
} info_settings_data_t;

static info_settings_data_t info_settings = {0};

/* ── WiFi IP 供外部调用（page_select 用 extern 引用） ── */
void get_wifi_ip_string(char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) return;

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        snprintf(buf, buf_size, "IP: 未连接");
        return;
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(netif, &ip_info);
    if (ret != ESP_OK || ip_info.ip.addr == 0) {
        snprintf(buf, buf_size, "IP: 未连接");
        return;
    }

    snprintf(buf, buf_size, "IP: " IPSTR, IP2STR(&ip_info.ip));
}

/* ── 蓝牙控制 ── */
static void ble_set(bool enable)
{
    if (enable) {
        app_ble_trigger_xbox_scan();
    } else {
        ESP_LOGI(TAG, "Bluetooth stopped");
    }
}

static void update_right_ble_item(void)
{
    if (!info_settings.ble_item || !info_settings.ble_label) return;

    lv_obj_set_style_bg_color(info_settings.ble_item,
                              lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_bg_opa(info_settings.ble_item, LV_OPA_30, 0);
    lv_obj_set_style_border_color(info_settings.ble_item,
                                  lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_border_width(info_settings.ble_item, 2, 0);
    lv_obj_set_style_text_color(info_settings.ble_label,
                                lv_color_white(), 0);

    if (info_settings.ble_switch) {
        if (info_settings.ble_on)
            lv_obj_add_state(info_settings.ble_switch, LV_STATE_CHECKED);
        else
            lv_obj_clear_state(info_settings.ble_switch, LV_STATE_CHECKED);
    }
}

/* ═══════════════════════════════════════════
 *  左屏 — INFO
 *
 *  移除了 SOC / WiFi IP，布局重新居中：
 *
 *   y=top+14   INFO  (蓝色标题)
 *   ─────────────────────────  (分隔线)
 *   y=center-24  粒子微动  (白色，主字)
 *   y=center- 4  coucou & 火柴棍  (灰色)
 *   ─────────────────────────
 *   y=center+20  版本号  (蓝灰)
 *   y=bottom-14  长按返回  (hint)
 *
 * ═══════════════════════════════════════════ */
static void create_left_screen(lv_obj_t *parent)
{
    if (!parent) return;

    info_settings.left_container = lv_obj_create(parent);
    lv_obj_remove_style_all(info_settings.left_container);
    lv_obj_set_size(info_settings.left_container, 200, 200);
    lv_obj_set_style_radius(info_settings.left_container, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(info_settings.left_container,
                              lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(info_settings.left_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(info_settings.left_container, 2, 0);
    lv_obj_set_style_border_color(info_settings.left_container,
                                  lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_clip_corner(info_settings.left_container, true, 0);
    lv_obj_align(info_settings.left_container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(info_settings.left_container, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题 */
    info_settings.left_title_label =
        lv_label_create(info_settings.left_container);
    lv_label_set_text(info_settings.left_title_label, "INFO");
    lv_obj_set_style_text_color(info_settings.left_title_label,
                                lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_text_font(info_settings.left_title_label,
                               &lv_font_montserrat_14, 0);
    lv_obj_align(info_settings.left_title_label, LV_ALIGN_TOP_MID, 0, 16);

    /* 分隔线 1 */
    lv_obj_t *sep1 = lv_obj_create(info_settings.left_container);
    lv_obj_remove_style_all(sep1);
    lv_obj_set_size(sep1, 130, 1);
    lv_obj_set_style_bg_color(sep1, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_opa(sep1, LV_OPA_COVER, 0);
    lv_obj_align(sep1, LV_ALIGN_TOP_MID, 0, 38);

    /* 作者主名 */
    info_settings.author_label =
        lv_label_create(info_settings.left_container);
    lv_label_set_text(info_settings.author_label, "粒子微动");
    lv_obj_set_style_text_color(info_settings.author_label,
                                lv_color_white(), 0);
    lv_obj_set_style_text_font(info_settings.author_label,
                               &lv_font_notosans_cs_medium_14, 0);
    lv_obj_set_style_text_align(info_settings.author_label,
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(info_settings.author_label, 160);
    lv_obj_align(info_settings.author_label, LV_ALIGN_CENTER, 0, -22);

    /* 作者副名 */
    lv_obj_t *sub = lv_label_create(info_settings.left_container);
    lv_label_set_text(sub, "coucou & 火柴棍");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_font(sub, &lv_font_notosans_cs_medium_14, 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(sub, 160);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 0);

    /* 分隔线 2 */
    lv_obj_t *sep2 = lv_obj_create(info_settings.left_container);
    lv_obj_remove_style_all(sep2);
    lv_obj_set_size(sep2, 130, 1);
    lv_obj_set_style_bg_color(sep2, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_opa(sep2, LV_OPA_COVER, 0);
    lv_obj_align(sep2, LV_ALIGN_CENTER, 0, 18);

    /* 版本号 */
    info_settings.version_label =
        lv_label_create(info_settings.left_container);
    lv_label_set_text(info_settings.version_label, APP_UI_VERSION);
    lv_obj_set_style_text_color(info_settings.version_label,
                                lv_color_hex(0x3B82F6), 0);
    lv_obj_set_style_text_font(info_settings.version_label,
                               &lv_font_montserrat_14, 0);
    lv_obj_align(info_settings.version_label, LV_ALIGN_CENTER, 0, 34);

    /* 长按提示 */
    lv_obj_t *hint = lv_label_create(info_settings.left_container);
    lv_label_set_text(hint, "\xe9\x95\xbf\xe6\x8c\x89\xe8\xbf\x94\xe5\x9b\x9e");  /* 长按返回 */
    lv_obj_set_style_text_color(hint, lv_color_hex(0x404040), 0);
    lv_obj_set_style_text_font(hint, &lv_font_notosans_cs_medium_14, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -18);
}

/* 右屏不变，保留蓝牙开关 */
static void create_right_screen(lv_obj_t *parent)
{
    if (!parent) return;

    info_settings.right_container = lv_obj_create(parent);
    lv_obj_remove_style_all(info_settings.right_container);
    lv_obj_set_size(info_settings.right_container, 200, 200);
    lv_obj_set_style_radius(info_settings.right_container,
                            LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(info_settings.right_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info_settings.right_container, 2, 0);
    lv_obj_set_style_border_color(info_settings.right_container,
                                  lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_clip_corner(info_settings.right_container, true, 0);
    lv_obj_align(info_settings.right_container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(info_settings.right_container, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题 */
    info_settings.right_title_label =
        lv_label_create(info_settings.right_container);
    lv_label_set_text(info_settings.right_title_label, "设置");
    lv_obj_set_style_text_color(info_settings.right_title_label,
                                lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_text_font(info_settings.right_title_label,
                               &lv_font_notosans_cs_medium_14, 0);
    lv_obj_align(info_settings.right_title_label, LV_ALIGN_TOP_MID, 0, 14);

    /* 蓝牙项 */
    const int item_w = 110;
    const int item_h = 46;

    info_settings.ble_item = lv_obj_create(info_settings.right_container);
    lv_obj_remove_style_all(info_settings.ble_item);
    lv_obj_set_size(info_settings.ble_item, item_w, item_h);
    lv_obj_align(info_settings.ble_item, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(info_settings.ble_item, 10, 0);
    lv_obj_set_style_bg_color(info_settings.ble_item,
                              lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_pad_hor(info_settings.ble_item, 10, 0);
    lv_obj_set_style_pad_ver(info_settings.ble_item, 0, 0);
    lv_obj_clear_flag(info_settings.ble_item, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(info_settings.ble_item, LV_OBJ_FLAG_SCROLLABLE);

    info_settings.ble_label = lv_label_create(info_settings.ble_item);
    lv_label_set_text(info_settings.ble_label, "蓝牙");
    lv_obj_set_style_text_font(info_settings.ble_label,
                               &lv_font_notosans_cs_medium_14, 0);
    lv_obj_align(info_settings.ble_label, LV_ALIGN_LEFT_MID, 0, 0);

    info_settings.ble_switch = lv_switch_create(info_settings.ble_item);
    lv_obj_set_size(info_settings.ble_switch, 42, 24);
    lv_obj_set_style_bg_color(info_settings.ble_switch,
                              lv_color_hex(0x404040), LV_PART_MAIN);
    lv_obj_set_style_bg_color(info_settings.ble_switch,
                              lv_palette_main(LV_PALETTE_BLUE),
                              LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(info_settings.ble_switch,
                            LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(info_settings.ble_switch,
                              lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(info_settings.ble_switch,
                            LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(info_settings.ble_switch,
                            LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(info_settings.ble_switch,
                                  4, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_color(info_settings.ble_switch,
                                  lv_color_hex(0x000000), LV_PART_INDICATOR);
    lv_obj_set_style_shadow_opa(info_settings.ble_switch,
                                LV_OPA_30, LV_PART_INDICATOR);
    lv_obj_align(info_settings.ble_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_clear_flag(info_settings.ble_switch, LV_OBJ_FLAG_CLICKABLE);

    /* 双击提示 */
    lv_obj_t *ble_hint = lv_label_create(info_settings.right_container);
    lv_label_set_text(ble_hint,
                      "\xe5\x8f\x8c\xe5\x87\xbb\xe5\x88\x87\xe6\x8d\xa2"); /* 双击切换 */
    lv_obj_set_style_text_font(ble_hint, &lv_font_notosans_cs_medium_14, 0);
    lv_obj_set_style_text_color(ble_hint, lv_color_hex(0x404040), 0);
    lv_obj_align(ble_hint, LV_ALIGN_BOTTOM_MID, 0, -18);

    update_right_ble_item();
}

void info_toggle_ble(void)
{
    info_settings.ble_on = !info_settings.ble_on;
    ble_set(info_settings.ble_on);
    update_right_ble_item();
    ESP_LOGI(TAG, "Toggle BLE: %s", info_settings.ble_on ? "ON" : "OFF");
}

static int info_settings_load(void)
{
    lv_obj_t *screen_left  = screen_manager.screens[DISPLAY_LEFT].root;
    lv_obj_t *screen_right = screen_manager.screens[DISPLAY_RIGHT].root;

    create_left_screen(screen_left);
    create_right_screen(screen_right);

    return 0;
}

static int info_settings_update(void)
{
    lv_timer_handler();
    return 0;
}

static int info_settings_unload(void)
{
    if (info_settings.left_container) {
        lv_obj_del(info_settings.left_container);
        info_settings.left_container = NULL;
    }
    if (info_settings.right_container) {
        lv_obj_del(info_settings.right_container);
        info_settings.right_container = NULL;
    }
    return 0;
}

screen_interface_t info_settings_screen_interface = {
    .load         = info_settings_load,
    .update       = info_settings_update,
    .unload       = info_settings_unload,
    .on_key_event = logic_page_info_on_key,
};