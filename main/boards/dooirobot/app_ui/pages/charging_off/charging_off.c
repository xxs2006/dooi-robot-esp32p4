// charging_off.c  —— 关机后仍在充电时显示的"关机充电"页面
//
//  进入条件：关机（长按电源键触发 on_power_shutdown）时若正处于充电状态，
//           不切断整机电源，而是进入本页面（禁止任何操作）。
//  退出条件：再次长按电源键 → 退出到默认"眼睛"界面（开机）；
//            充电被拔出（停止充电）→ 直接断电关机（保持用户关机意愿）。
//
//  左屏：复用 info.c 的 INFO 左屏布局（标题 / 作者 / 版本）
//  右屏：CHARGING 标题 + 电量充电状态，设计参考 page_select.c 的充电指示灯风格，
//        电池竖排（旋转 180°：正极朝上），电池格自上而下排列。
//
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include <lvgl.h>
#include "app_ui.h"
#include "power_manager.h"

static const char *TAG = "charging_off";

LV_FONT_DECLARE(lv_font_notosans_cs_medium_14);

/* ── 统一配色 ── */
#define C_BG       0x000000
#define C_BORDER   0x3B82F6
#define C_ACCENT   0x3B82F6
#define C_ACCENT2  0x60A5FA
#define C_DIM      0x808080
#define C_HINT     0x404040
#define C_SEG_OFF  0x2A2A2A
#define C_CHARGING 0xFACC15   /* 充电状态色（黄） */

typedef struct {
    /* 左屏 */
    lv_obj_t *left_container;
    lv_obj_t *left_title_label;
    lv_obj_t *author_label;
    lv_obj_t *version_label;

    /* 右屏 */
    lv_obj_t *right_container;
    lv_obj_t *right_title_label;
    lv_obj_t *bat_frame;          /* 竖排电池外框 */
    lv_obj_t *bat_segments[5];    /* 竖排电池格：自上而下 */
    lv_obj_t *bat_tip;            /* 顶部正极 */
    lv_obj_t *bat_pct_label;

    uint32_t blink_ms;            /* 充电闪烁计时累计 */
} charging_off_data_t;

static charging_off_data_t charging_off = {0};

static lv_obj_t *s_bat_frame_ref = NULL;   /* 外框引用，用于更新边框色 */

/* ── 工具 ── */
static void setup_screen_bg(lv_obj_t *screen)
{
    if (!screen) return;
    lv_obj_set_style_bg_color(screen, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}

static void setup_circle_panel(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, 200, 200);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_clip_corner(obj, true, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_align(obj, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

static lv_color_t bat_color(uint8_t level)
{
    if (level > 60) return lv_palette_main(LV_PALETTE_GREEN);
    if (level > 20) return lv_palette_main(LV_PALETTE_ORANGE);
    return lv_palette_main(LV_PALETTE_RED);
}

/* ═══════════════════════════════════════════
 *  左屏 — INFO（复用 info.c 的 INFO 布局）
 *
 *   y=top+16  INFO  (蓝色标题)
 *   ───────────────  (分隔线)
 *   粒子微动      (白色，主字)
 *   coucou & 火柴棍 (灰色)
 *   ───────────────
 *   版本号         (蓝灰)
 *   bottom-18  长按返回 (hint)
 * ═══════════════════════════════════════════ */
static void create_left_screen(lv_obj_t *parent)
{
    if (!parent) return;

    charging_off.left_container = lv_obj_create(parent);
    setup_circle_panel(charging_off.left_container);

    /* 标题 */
    charging_off.left_title_label =
        lv_label_create(charging_off.left_container);
    lv_label_set_text(charging_off.left_title_label, "INFO");
    lv_obj_set_style_text_color(charging_off.left_title_label,
                                lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_text_font(charging_off.left_title_label,
                               &lv_font_montserrat_14, 0);
    lv_obj_align(charging_off.left_title_label, LV_ALIGN_TOP_MID, 0, 16);

    /* 分隔线 1 */
    lv_obj_t *sep1 = lv_obj_create(charging_off.left_container);
    lv_obj_remove_style_all(sep1);
    lv_obj_set_size(sep1, 130, 1);
    lv_obj_set_style_bg_color(sep1, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_opa(sep1, LV_OPA_COVER, 0);
    lv_obj_align(sep1, LV_ALIGN_TOP_MID, 0, 38);

    /* 作者主名 */
    charging_off.author_label =
        lv_label_create(charging_off.left_container);
    lv_label_set_text(charging_off.author_label, "粒子微动");
    lv_obj_set_style_text_color(charging_off.author_label,
                                lv_color_white(), 0);
    lv_obj_set_style_text_font(charging_off.author_label,
                               &lv_font_notosans_cs_medium_14, 0);
    lv_obj_set_style_text_align(charging_off.author_label,
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(charging_off.author_label, 160);
    lv_obj_align(charging_off.author_label, LV_ALIGN_CENTER, 0, -22);

    /* 作者副名 */
    lv_obj_t *sub = lv_label_create(charging_off.left_container);
    lv_label_set_text(sub, "coucou & 火柴棍");
    lv_obj_set_style_text_color(sub, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_text_font(sub, &lv_font_notosans_cs_medium_14, 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(sub, 160);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 0);

    /* 分隔线 2 */
    lv_obj_t *sep2 = lv_obj_create(charging_off.left_container);
    lv_obj_remove_style_all(sep2);
    lv_obj_set_size(sep2, 130, 1);
    lv_obj_set_style_bg_color(sep2, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_opa(sep2, LV_OPA_COVER, 0);
    lv_obj_align(sep2, LV_ALIGN_CENTER, 0, 18);

    /* 版本号 */
    charging_off.version_label =
        lv_label_create(charging_off.left_container);
    lv_label_set_text(charging_off.version_label, APP_UI_VERSION);
    lv_obj_set_style_text_color(charging_off.version_label,
                                lv_color_hex(0x3B82F6), 0);
    lv_obj_set_style_text_font(charging_off.version_label,
                               &lv_font_montserrat_14, 0);
    lv_obj_align(charging_off.version_label, LV_ALIGN_CENTER, 0, 34);
}

/* ═══════════════════════════════════════════
 *  右屏 — 横排电池充电显示
 *
 *  参考 page_select.c 的左屏充电设计，将电池横排（正极朝右）：
 *
 *   y=top+16    CHARGING 标题（黄，与左屏 INFO 对齐）
 *   y=top+38    下划线（与左屏 INFO 下划线一致）
 *   y=top+~     [横排电池外框]（垂直居中偏上区域）
 *                  右侧为"正极"突起，5 格从左到右居中排列
 *   y=~         百分比（电池下方居中）
 *
 *  电池格横向堆叠：越靠左代表已充满的电量越多（电量高时左侧多亮）。
 * ═══════════════════════════════════════════ */
static void create_right_screen(lv_obj_t *parent)
{
    if (!parent) return;

    charging_off.right_container = lv_obj_create(parent);
    setup_circle_panel(charging_off.right_container);

    /* 标题：与左屏 INFO 对齐（TOP_MID, y=16） */
    charging_off.right_title_label =
        lv_label_create(charging_off.right_container);
    lv_label_set_text(charging_off.right_title_label, "CHARGING");
    lv_obj_set_style_text_color(charging_off.right_title_label,
                                lv_color_hex(C_CHARGING), 0);
    lv_obj_set_style_text_font(charging_off.right_title_label,
                               &lv_font_montserrat_14, 0);
    lv_obj_align(charging_off.right_title_label, LV_ALIGN_TOP_MID, 0, 16);

    /* 标题下划线：与左屏 INFO 下划线一致（TOP_MID, y=38，宽 130） */
    lv_obj_t *sep = lv_obj_create(charging_off.right_container);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, 130, 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_align(sep, LV_ALIGN_TOP_MID, 0, 38);

    /* ── 横排电池（正极朝右，电芯从左到右）──
     *  外框：宽 88 高 46，中心相对面板中心 (0, -14)
     *  电芯相对外框严格居中：不设 padding，直接用 frame 原点坐标
     */
    const int BAT_W    = 88;
    const int BAT_H    = 46;
    const int SEG_W    = 12;
    const int SEG_H    = 34;
    const int SEG_GAP  = 3;
    const int BAT_CX   = 0;   /* 外框中心 x 偏移 */
    const int BAT_CY   = -14; /* 外框中心 y 偏移（居中偏上，给下方百分比留空间） */

    /* 外框 */
    charging_off.bat_frame = lv_obj_create(charging_off.right_container);
    lv_obj_remove_style_all(charging_off.bat_frame);
    lv_obj_set_size(charging_off.bat_frame, BAT_W, BAT_H);
    lv_obj_set_style_radius(charging_off.bat_frame, 3, 0);
    lv_obj_set_style_border_width(charging_off.bat_frame, 1, 0);
    lv_obj_set_style_border_color(charging_off.bat_frame,
                                  lv_color_hex(0x505050), 0);
    lv_obj_set_style_bg_color(charging_off.bat_frame,
                              lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(charging_off.bat_frame, LV_OPA_COVER, 0);
    /* 不设 padding，电芯坐标以 frame 原点为准，便于精确居中 */
    lv_obj_clear_flag(charging_off.bat_frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(charging_off.bat_frame, LV_ALIGN_CENTER, BAT_CX, BAT_CY);
    s_bat_frame_ref = charging_off.bat_frame;

    /* 电芯相对外框居中：
     *  水平：5 格总宽 segs_w，水平居中后左侧起始 x = (BAT_W - segs_w) / 2
     *  垂直：y = (BAT_H - SEG_H) / 2
     */
    const int segs_w = 5 * SEG_W + 4 * SEG_GAP;   /* 5 格总宽 = 72 */
    const int seg_x0 = (BAT_W - segs_w) / 2;      /* 左侧起始 x = 8 */
    const int seg_y  = (BAT_H - SEG_H) / 2;       /* 垂直居中 y = 6 */

    /* 5 格，从左到右排列（最左侧一格为 index 0，电量高时左侧多亮） */
    for (int i = 0; i < 5; i++) {
        charging_off.bat_segments[i] =
            lv_obj_create(charging_off.bat_frame);
        lv_obj_remove_style_all(charging_off.bat_segments[i]);
        lv_obj_set_size(charging_off.bat_segments[i], SEG_W, SEG_H);
        lv_obj_set_style_radius(charging_off.bat_segments[i], 2, 0);
        lv_obj_set_style_bg_color(charging_off.bat_segments[i],
                                  lv_color_hex(C_SEG_OFF), 0);
        lv_obj_set_style_bg_opa(charging_off.bat_segments[i], LV_OPA_COVER, 0);
        lv_obj_set_pos(charging_off.bat_segments[i],
                       seg_x0 + i * (SEG_W + SEG_GAP),
                       seg_y);
    }

    /* 正极突起：右侧外框外 +3px，垂直居中 */
    charging_off.bat_tip = lv_obj_create(charging_off.right_container);
    lv_obj_remove_style_all(charging_off.bat_tip);
    lv_obj_set_size(charging_off.bat_tip, 3, 8);
    lv_obj_set_style_radius(charging_off.bat_tip, 1, 0);
    lv_obj_set_style_bg_color(charging_off.bat_tip,
                              lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_style_bg_opa(charging_off.bat_tip, LV_OPA_COVER, 0);
    lv_obj_align(charging_off.bat_tip, LV_ALIGN_CENTER,
                 BAT_CX + BAT_W / 2 + 3, BAT_CY);

    /* 百分比：电池外框下方 */
    charging_off.bat_pct_label =
        lv_label_create(charging_off.right_container);
    lv_label_set_text(charging_off.bat_pct_label, "100%");
    lv_obj_set_style_text_font(charging_off.bat_pct_label,
                               &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(charging_off.bat_pct_label,
                                lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_align(charging_off.bat_pct_label, LV_ALIGN_CENTER,
                 BAT_CX, BAT_CY + BAT_H / 2 + 18);
}

/* ── 充电渲染：横排电池 ── */
static void charging_off_battery_render(void)
{
    uint8_t    level    = power_manager_get_level();
    bool       charging = power_manager_is_charging();
    lv_color_t col      = bat_color(level);

    /* 点亮格数（按电量五等分），从左到右 */
    int lit = (level + 19) / 20;
    if (lit > 5) lit = 5;
    if (lit < 0) lit = 0;
    int lit_disp = (lit > 0) ? lit : 1;

    /* 充电闪烁相位 */
    bool edge_on = true;
    if (charging) {
        uint32_t half   = 1400 - (uint32_t)level * 10;
        uint32_t period = half * 2;
        uint32_t phase  = charging_off.blink_ms % period;
        edge_on = (phase < half);
    }

    for (int i = 0; i < 5; i++) {
        if (!charging_off.bat_segments[i]) continue;
        lv_color_t c;
        if (charging && i == lit_disp - 1) {
            /* 边界格：充电时闪烁 */
            c = edge_on ? col : lv_color_hex(C_SEG_OFF);
        } else {
            c = (i < lit) ? col : lv_color_hex(C_SEG_OFF);
        }
        lv_obj_set_style_bg_color(charging_off.bat_segments[i], c, 0);
    }

    /* 正极 */
    if (charging_off.bat_tip) {
        lv_color_t tip = (charging && !edge_on)
                         ? lv_color_hex(C_SEG_OFF) : col;
        lv_obj_set_style_bg_color(charging_off.bat_tip, tip, 0);
    }

    /* 外框边框：充电中=黄色 */
    if (s_bat_frame_ref) {
        lv_obj_set_style_border_color(
            s_bat_frame_ref,
            charging ? lv_color_hex(C_CHARGING)
                     : lv_color_hex(0x505050),
            0);
    }

    /* 百分比 */
    if (charging_off.bat_pct_label) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%d%%", level);
        lv_label_set_text(charging_off.bat_pct_label, buf);
        lv_obj_set_style_text_color(charging_off.bat_pct_label, col, 0);
    }
}

/* ── 页面接口 ── */
static int charging_off_load(void)
{
    lv_obj_t *screen_left  = screen_manager.screens[DISPLAY_LEFT].root;
    lv_obj_t *screen_right = screen_manager.screens[DISPLAY_RIGHT].root;

    if (!screen_left || !screen_right) {
        ESP_LOGE(TAG, "Screen root is NULL");
        return -1;
    }

    memset(&charging_off, 0, sizeof(charging_off_data_t));

    setup_screen_bg(screen_left);
    setup_screen_bg(screen_right);

    create_left_screen(screen_left);
    create_right_screen(screen_right);

    charging_off_battery_render();

    ESP_LOGI(TAG, "Charging-off screen loaded, level=%d%% charging=%d",
             power_manager_get_level(), power_manager_is_charging());
    return 0;
}

static int charging_off_update(void)
{
    charging_off.blink_ms += APP_UI_TICK_MS;
    charging_off_battery_render();
    lv_timer_handler();
    return 0;
}

static int charging_off_unload(void)
{
    if (charging_off.left_container) {
        lv_obj_del(charging_off.left_container);
        charging_off.left_container = NULL;
    }
    if (charging_off.right_container) {
        lv_obj_del(charging_off.right_container);
        charging_off.right_container = NULL;
    }

    charging_off.left_title_label = NULL;
    charging_off.author_label     = NULL;
    charging_off.version_label    = NULL;

    charging_off.right_title_label = NULL;
    charging_off.bat_frame         = NULL;
    for (int i = 0; i < 5; i++) charging_off.bat_segments[i] = NULL;
    charging_off.bat_tip       = NULL;
    charging_off.bat_pct_label = NULL;
    s_bat_frame_ref = NULL;

    return 0;
}

screen_interface_t charging_off_screen_interface = {
    .load         = charging_off_load,
    .update       = charging_off_update,
    .unload       = charging_off_unload,
    .on_key_event = logic_page_charging_off_on_key,
};
