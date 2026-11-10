// page_select.c  —— 修正布局版
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include <lvgl.h>
#include "app_ui.h"

extern void get_wifi_ip_string(char *buf, size_t buf_size);

static const char *TAG = "page_select";

LV_FONT_DECLARE(lv_font_notosans_cs_medium_14);

typedef struct {
    screen_type_t type;
    const char   *name;
    bool          available;
} page_info_t;

static const page_info_t page_list[] = {
    {SCREEN_EYES,           "眼睛",      true},
    {SCREEN_DIGITAL_CLOCK,  "时钟",      true},
    {SCREEN_CAMERA,         "相机",      true},
    {SCREEN_LUCKY_CAT,      "招财猫",    true},
    {SCREEN_MUSIC_SPECTRUM, "音乐",      true},
    {SCREEN_INFO,           "关于/设置", true},
};
#define PAGE_COUNT (sizeof(page_list) / sizeof(page_info_t))

#define C_BG       0x000000
#define C_BORDER   0x3B82F6
#define C_ACCENT   0x3B82F6
#define C_ACCENT2  0x60A5FA
#define C_DIM      0x606060
#define C_HINT     0x404040
#define C_ITEM_BG  0x1C1C1E
#define C_ITEM_SEL 0x1D3A6B
#define C_SEP      0x2A2A2A

typedef struct {
    /* 左屏 */
    lv_obj_t *left_container;
    lv_obj_t *ip_label;          
    lv_obj_t *page_name_label;
    lv_obj_t *page_index_label;
    lv_obj_t *bat_segments[5];
    lv_obj_t *bat_tip;
    lv_obj_t *bat_pct_label;
    lv_obj_t *bat_charge_label;
    lv_obj_t *hint_label;

    /* 右屏 */
    lv_obj_t *right_container;
    lv_obj_t *items[PAGE_COUNT];
    lv_obj_t *item_bars[PAGE_COUNT];
    lv_obj_t *item_labels[PAGE_COUNT];

    int  selected_index;
    bool needs_refresh;

    uint32_t blink_ms;    /* 充电闪烁计时累计（毫秒） */
} page_select_data_t;

static page_select_data_t ps = {0};

static lv_obj_t *s_bat_frame_ref = NULL;   /* 保存外框引用以便更新边框色 */

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
}

static lv_color_t bat_color(uint8_t level)
{
    if (level > 60) return lv_palette_main(LV_PALETTE_GREEN);
    if (level > 20) return lv_palette_main(LV_PALETTE_ORANGE);
    return lv_palette_main(LV_PALETTE_RED);
}

static int find_next(int start, int dir)
{
    if (PAGE_COUNT == 0) return 0;
    int idx = start;
    for (int n = 0; n < (int)PAGE_COUNT; n++) {
        idx += dir;
        if (idx < 0)                idx = (int)PAGE_COUNT - 1;
        if (idx >= (int)PAGE_COUNT) idx = 0;
        if (page_list[idx].available) return idx;
    }
    return start;
}
/* ── 电池渲染（每次调用完整刷新）──
 *
 *  充电中（充电宝指示灯风格）：
 *   - 已充满的格子常亮，当前正在充入的“边界格”按电量决定的节奏闪烁；
 *   - 电量越低闪得越快，电量越高闪得越慢。
 *  未充电：格子按电量三档常亮。
 *
 *  充电/不充电状态文本单独占电池框下一行。
 */
static void page_select_battery_render(void)
{
    uint8_t    level    = power_manager_get_level();
    bool       charging = power_manager_is_charging();
    lv_color_t col      = bat_color(level);

    /* 点亮格数（按电量五等分） */
    int lit = (level + 19) / 20;
    if (lit > 5) lit = 5;
    if (lit < 0) lit = 0;
    int lit_disp = (lit > 0) ? lit : 1;   /* 充电中至少保持 1 格闪烁 */

    /* 充电闪烁相位：半周期随电量变化（低电量快，高电量慢） */
    bool edge_on = true;
    if (charging) {
        uint32_t half   = 1400 - (uint32_t)level * 10; /* 1400ms(0%) ~ 400ms(100%) */
        uint32_t period = half * 2;
        uint32_t phase  = ps.blink_ms % period;
        edge_on = (phase < half);
    }

    /* 5 格 */
    for (int i = 0; i < 5; i++) {
        if (!ps.bat_segments[i]) continue;
        lv_color_t c;
        if (charging && i == lit_disp - 1) {
            /* 边界格：充电时闪烁 */
            c = edge_on ? col : lv_color_hex(0x2A2A2A);
        } else {
            c = (i < lit) ? col : lv_color_hex(0x2A2A2A);
        }
        lv_obj_set_style_bg_color(ps.bat_segments[i], c, 0);
    }

    /* 正极：充电时随边界格一起闪，未充电跟随电量 */
    if (ps.bat_tip) {
        lv_color_t tip = (charging && !edge_on) ? lv_color_hex(0x2A2A2A) : col;
        lv_obj_set_style_bg_color(ps.bat_tip, tip, 0);
    }

    /* 外框边框：充电中=黄色，未充电=灰色 */
    if (s_bat_frame_ref) {
        lv_obj_set_style_border_color(
            s_bat_frame_ref,
            charging ? lv_palette_main(LV_PALETTE_YELLOW)
                     : lv_color_hex(0x505050),
            0);
    }

    /* 百分比（颜色跟随电量） */
    if (ps.bat_pct_label) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%d%%", level);
        lv_label_set_text(ps.bat_pct_label, buf);
        lv_obj_set_style_text_color(ps.bat_pct_label, col, 0);
    }

    /* 充电状态行（电池框下一行） */
    if (ps.bat_charge_label) {
        if (charging) {
            lv_label_set_text(ps.bat_charge_label, "CHARGING");
            lv_obj_set_style_text_color(ps.bat_charge_label,
                                        lv_palette_main(LV_PALETTE_YELLOW), 0);
        } else {
            lv_label_set_text(ps.bat_charge_label, "NOT CHARGING");
            lv_obj_set_style_text_color(ps.bat_charge_label,
                                        lv_color_hex(0x505050), 0);
        }
    }
}

/* 供 needs_refresh 时的旧接口调用 */
static void update_battery(void)
{
    page_select_battery_render();
}

static void update_ip(void)
{
    if (!ps.ip_label) return;
    char buf[40];
    get_wifi_ip_string(buf, sizeof(buf));

    /* 去掉 "IP: " 前缀，拼上图标 */
    const char *ip_text = buf;
    if (strncmp(buf, "IP: ", 4) == 0) ip_text = buf + 4;

    char display[48];
    snprintf(display, sizeof(display), "IP: %s", ip_text);
    lv_label_set_text(ps.ip_label, display);

    bool connected = (strstr(buf, "未连接") == NULL);
    lv_obj_set_style_text_color(
        ps.ip_label,
        connected ? lv_palette_main(LV_PALETTE_GREEN)
                  : lv_color_hex(C_DIM),
        0);
}

static void update_left_display(void)
{
    if (ps.selected_index < 0 ||
        ps.selected_index >= (int)PAGE_COUNT) return;

    if (ps.page_name_label)
        lv_label_set_text(ps.page_name_label,
                          page_list[ps.selected_index].name);

    if (ps.page_index_label) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d / %d",
                 ps.selected_index + 1, (int)PAGE_COUNT);
        lv_label_set_text(ps.page_index_label, buf);
    }

    update_battery();
    update_ip();
}

static void update_right_items(void)
{
    for (int i = 0; i < (int)PAGE_COUNT; i++) {
        if (!ps.items[i]) continue;
        bool sel   = (i == ps.selected_index);
        bool avail = page_list[i].available;

        lv_obj_set_style_bg_color(
            ps.items[i],
            lv_color_hex(sel ? C_ITEM_SEL : C_ITEM_BG), 0);

        if (ps.item_bars[i]) {
            lv_obj_set_style_bg_color(
                ps.item_bars[i],
                sel ? lv_color_hex(C_ACCENT) : lv_color_hex(C_ITEM_BG), 0);
        }

        if (ps.item_labels[i]) {
            lv_color_t tc;
            if      (sel)   tc = lv_color_white();
            else if (avail) tc = lv_color_hex(0xAAAAAA);
            else            tc = lv_color_hex(0x444444);
            lv_obj_set_style_text_color(ps.item_labels[i], tc, 0);
        }
    }
}

/* ═══════════════════════════════════════════
 *  左屏布局（面板 200×200，圆形裁切）
 *
 *  坐标以面板左上角为原点，CENTER = (100,100)
 *  圆形安全区（留 8px 内边距）：x∈[30,170]  y∈[30,170]
 *
 *  固定绝对 y（相对面板顶部）：
 *   y=30   WiFi 单行标签，居中，宽 110
 *   y=46   分割线，宽 120
 *   y=62   页面名（主字），居中，宽 140
 *   y=84   页码，居中
 *   y=100  分割线，宽 120
 *   y=116  电池行（外框左边 x=38，宽 57+3=60，百分比+充电在右）
 *   y=148  提示，居中，宽 130
 *
 * ═══════════════════════════════════════════ */
static void create_left_screen(lv_obj_t *parent)
{
    if (!parent) return;

    ps.left_container = lv_obj_create(parent);
    setup_circle_panel(ps.left_container);

    /* ── WiFi 行（单标签居中，宽度受限防溢出）── */
    ps.ip_label = lv_label_create(ps.left_container);
    lv_label_set_text(ps.ip_label, "IP: ...");
    lv_obj_set_style_text_font(ps.ip_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ps.ip_label, lv_color_hex(C_DIM), 0);
    lv_label_set_long_mode(ps.ip_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(ps.ip_label, 120);
    lv_obj_set_style_text_align(ps.ip_label, LV_TEXT_ALIGN_CENTER, 0);
    /* TOP_MID x=0, y=22 → 绝对 y=22，文字高≈14，底边 y=36；安全 */
    lv_obj_align(ps.ip_label, LV_ALIGN_TOP_MID, 0, 22);

    /* ── 分割线 1，y=42 ── */
    lv_obj_t *sep1 = lv_obj_create(ps.left_container);
    lv_obj_remove_style_all(sep1);
    lv_obj_set_size(sep1, 120, 1);
    lv_obj_set_style_bg_color(sep1, lv_color_hex(C_SEP), 0);
    lv_obj_set_style_bg_opa(sep1, LV_OPA_COVER, 0);
    lv_obj_align(sep1, LV_ALIGN_TOP_MID, 0, 42);

    /* ── 页面名，CENTER y=-30 → 绝对 y=70 ── */
    ps.page_name_label = lv_label_create(ps.left_container);
    lv_label_set_text(ps.page_name_label, "");
    lv_obj_set_style_text_font(ps.page_name_label,
                               &lv_font_notosans_cs_medium_14, 0);
    lv_obj_set_style_text_color(ps.page_name_label, lv_color_white(), 0);
    lv_label_set_long_mode(ps.page_name_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(ps.page_name_label, 130);
    lv_obj_set_style_text_align(ps.page_name_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ps.page_name_label, LV_ALIGN_CENTER, 0, -30);

    /* ── 页码，CENTER y=-10 → 绝对 y=90 ── */
    ps.page_index_label = lv_label_create(ps.left_container);
    lv_label_set_text(ps.page_index_label, "1 / 6");
    lv_obj_set_style_text_font(ps.page_index_label,
                               &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ps.page_index_label,
                                lv_color_hex(C_ACCENT2), 0);
    lv_obj_align(ps.page_index_label, LV_ALIGN_CENTER, 0, -10);

    /* ── 分割线 2，CENTER y=+8 → 绝对 y=108 ── */
    lv_obj_t *sep2 = lv_obj_create(ps.left_container);
    lv_obj_remove_style_all(sep2);
    lv_obj_set_size(sep2, 120, 1);
    lv_obj_set_style_bg_color(sep2, lv_color_hex(C_SEP), 0);
    lv_obj_set_style_bg_opa(sep2, LV_OPA_COVER, 0);
    lv_obj_align(sep2, LV_ALIGN_CENTER, 0, 8);

    /* ── 电池区（CENTER y=+26 → 绝对 y=126）──
     *
     *  5格条外框：宽57 高16，整体水平居中（BAT_CX 取 −20 使整体居中性偏移为 0）
     *  正极：宽3 高8，紧贴外框右侧
     *  百分比：外框右侧同排
     *  充电状态行：单独占电池框下一行（居中）
     *
     *  水平分布（相对面板中心，BAT_CX=−20）：
     *    外框中心 x=−20  → 绝对左边 x=52  右边 x=108
     *    正极     x=−20+28+2=10  → x=110~113
     *    百分比   x=−20+28+25=33 → x=133，宽约28，右边 x=147  ✅
     */
    const int BAT_CX   = -20;   /* 外框中心相对面板中心的 x 偏移 */
    const int BAT_CY   =  26;   /* 外框中心相对面板中心的 y 偏移 */
    const int BAT_W    =  57;   /* 外框总宽 = 5*9+4*2+4(pad) */
    const int BAT_H    =  16;
    const int SEG_W    =   9;
    const int SEG_H    =   8;   /* 格内高 = BAT_H - 2*pad(4) */
    const int SEG_GAP  =   2;
    const int CHG_CY   =  42;   /* 充电状态行中心偏移：电池框下一行 */

    /* 外框 */
    lv_obj_t *bat_frame = lv_obj_create(ps.left_container);
    lv_obj_remove_style_all(bat_frame);
    lv_obj_set_size(bat_frame, BAT_W, BAT_H);
    lv_obj_set_style_radius(bat_frame, 3, 0);
    lv_obj_set_style_border_width(bat_frame, 1, 0);
    lv_obj_set_style_border_color(bat_frame, lv_color_hex(0x505050), 0);
    lv_obj_set_style_bg_color(bat_frame, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(bat_frame, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(bat_frame, 3, 0);
    lv_obj_clear_flag(bat_frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bat_frame, LV_ALIGN_CENTER, BAT_CX, BAT_CY);

    /* 5 格 */
    for (int i = 0; i < 5; i++) {
        ps.bat_segments[i] = lv_obj_create(bat_frame);
        lv_obj_remove_style_all(ps.bat_segments[i]);
        lv_obj_set_size(ps.bat_segments[i], SEG_W, SEG_H);
        lv_obj_set_style_radius(ps.bat_segments[i], 2, 0);
        lv_obj_set_style_bg_color(ps.bat_segments[i],
                                  lv_color_hex(0x2A2A2A), 0);
        lv_obj_set_style_bg_opa(ps.bat_segments[i], LV_OPA_COVER, 0);
        lv_obj_set_pos(ps.bat_segments[i],
                       i * (SEG_W + SEG_GAP), 0);
    }

    /* 正极突起，紧贴外框右边 +2px */
    ps.bat_tip = lv_obj_create(ps.left_container);
    lv_obj_remove_style_all(ps.bat_tip);
    lv_obj_set_size(ps.bat_tip, 3, 8);
    lv_obj_set_style_radius(ps.bat_tip, 1, 0);
    lv_obj_set_style_bg_color(ps.bat_tip,
                              lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_style_bg_opa(ps.bat_tip, LV_OPA_COVER, 0);
    lv_obj_align(ps.bat_tip, LV_ALIGN_CENTER,
                 BAT_CX + BAT_W / 2 + 3, BAT_CY);

    /* 百分比，外框右边 +12 */
    ps.bat_pct_label = lv_label_create(ps.left_container);
    lv_label_set_text(ps.bat_pct_label, "100%");
    lv_obj_set_style_text_font(ps.bat_pct_label,
                               &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ps.bat_pct_label,
                                lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_align(ps.bat_pct_label, LV_ALIGN_CENTER,
                 BAT_CX + BAT_W / 2 + 25, BAT_CY);

    /* 充电状态行：独立占电池框下一行，圆盘水平正中 */
    ps.bat_charge_label = lv_label_create(ps.left_container);
    lv_label_set_text(ps.bat_charge_label, "");
    lv_obj_set_style_text_font(ps.bat_charge_label,
                               &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ps.bat_charge_label,
                                lv_palette_main(LV_PALETTE_YELLOW), 0);
    lv_label_set_long_mode(ps.bat_charge_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(ps.bat_charge_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ps.bat_charge_label, LV_ALIGN_CENTER, 0, CHG_CY);

    /* ── 提示，BOTTOM_MID y=-26 → 绝对 y=160，安全 ── */
    ps.hint_label = lv_label_create(ps.left_container);
    lv_label_set_text(ps.hint_label,
                      "L\xe2\x86\x91 R\xe2\x86\x93  "
                      "\xe5\x8f\x8c\xe5\x87\xbb\xe7\xa1\xae\xe8\xae\xa4");
    lv_obj_set_style_text_font(ps.hint_label,
                               &lv_font_notosans_cs_medium_14, 0);
    lv_obj_set_style_text_color(ps.hint_label,
                                lv_color_hex(C_HINT), 0);
    lv_label_set_long_mode(ps.hint_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(ps.hint_label, 130);
    lv_obj_set_style_text_align(ps.hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ps.hint_label, LV_ALIGN_BOTTOM_MID, 0, -26);
}

/* ═══════════════════════════════════════════
 *  右屏布局
 *
 *  item_h=24, spacing=2, PAGE_COUNT=6
 *  total_h = 6*24 + 5*2 = 154
 *  标题区高度 = 40px（y=0~40）
 *  剩余可用 = 200-40 = 160，列表 154px 正好放下
 *  list_top = 40 + (160-154)/2 = 43
 *  最后项底边 = 43 + 154 = 197  ✅（刚好在圆内）
 *
 *  item_w=116, list_x=(200-116)/2=42
 *  item 左边 x=42，右边 x=158  ✅（圆形内安全）
 *
 * ═══════════════════════════════════════════ */
static void create_right_screen(lv_obj_t *parent)
{
    if (!parent) return;

    ps.right_container = lv_obj_create(parent);
    setup_circle_panel(ps.right_container);

    /* 标题 */
    lv_obj_t *title = lv_label_create(ps.right_container);
    lv_label_set_text(title, "PAGES");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_ACCENT2), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    /* 标题下分割线 */
    lv_obj_t *sep = lv_obj_create(ps.right_container);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, 110, 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(C_SEP), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_align(sep, LV_ALIGN_TOP_MID, 0, 28);

    /* 列表参数 */
    const int item_h  = 24;
    const int item_w  = 90;
    const int spacing = 2;
    const int bar_w   = 3;
    const int list_top = 30;           /* 标题区占 30px */
    const int total_h  = (int)PAGE_COUNT * item_h
                         + ((int)PAGE_COUNT - 1) * spacing; /* 154 */
    /* 在剩余 160px 内垂直居中 */
    const int offset  = (160 - total_h) / 2;               /* 3 */
    const int start_y = list_top + offset;                  /* 43 */
    const int list_x  = (200 - item_w) / 2;                /* 42 */

    for (int i = 0; i < (int)PAGE_COUNT; i++) {
        int iy = start_y + i * (item_h + spacing);

        /* 行容器 */
        ps.items[i] = lv_obj_create(ps.right_container);
        lv_obj_remove_style_all(ps.items[i]);
        lv_obj_set_size(ps.items[i], item_w, item_h);
        lv_obj_set_pos(ps.items[i], list_x, iy);
                lv_obj_set_style_radius(ps.items[i], 4, 0);
        lv_obj_set_style_bg_color(ps.items[i], lv_color_hex(C_ITEM_BG), 0);
        lv_obj_set_style_bg_opa(ps.items[i], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(ps.items[i], 0, 0);
        lv_obj_clear_flag(ps.items[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(ps.items[i], LV_OBJ_FLAG_SCROLLABLE);

        /* 左侧竖条 */
        ps.item_bars[i] = lv_obj_create(ps.items[i]);
        lv_obj_remove_style_all(ps.item_bars[i]);
        lv_obj_set_size(ps.item_bars[i], bar_w, item_h - 8);
        lv_obj_set_style_radius(ps.item_bars[i], 2, 0);
        lv_obj_set_style_bg_color(ps.item_bars[i],
                                  lv_color_hex(C_ITEM_BG), 0);
        lv_obj_set_style_bg_opa(ps.item_bars[i], LV_OPA_COVER, 0);
        lv_obj_align(ps.item_bars[i], LV_ALIGN_LEFT_MID, 4, 0);

        /* 页面名标签 */
        ps.item_labels[i] = lv_label_create(ps.items[i]);
        lv_label_set_text(ps.item_labels[i], page_list[i].name);
        lv_obj_set_style_text_font(ps.item_labels[i],
                                   &lv_font_notosans_cs_medium_14, 0);
        lv_obj_set_style_text_color(ps.item_labels[i],
                                    lv_color_hex(0xAAAAAA), 0);
        lv_label_set_long_mode(ps.item_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(ps.item_labels[i], item_w - bar_w - 14);
        lv_obj_set_style_text_align(ps.item_labels[i],
                                    LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(ps.item_labels[i], LV_ALIGN_CENTER, 4, 0);
    }
}

/* ═══════════════════════════════════════════
 *  页面接口
 * ═══════════════════════════════════════════ */

void page_select_move(int dir)
{
    ps.selected_index = find_next(ps.selected_index, dir);
    ps.needs_refresh  = true;
    ESP_LOGI(TAG, "Select %s: %s",
             dir < 0 ? "up" : "down",
             page_list[ps.selected_index].name);
}

void page_select_confirm(void)
{
    if (ps.selected_index >= 0 &&
        ps.selected_index < (int)PAGE_COUNT &&
        page_list[ps.selected_index].available) {
        screen_set_current(page_list[ps.selected_index].type);
    }
}

static int page_select_load(void)
{
    lv_obj_t *screen_left  = screen_manager.screens[DISPLAY_LEFT].root;
    lv_obj_t *screen_right = screen_manager.screens[DISPLAY_RIGHT].root;

    if (!screen_left || !screen_right) {
        ESP_LOGE(TAG, "Screen root is NULL");
        return -1;
    }

    memset(&ps, 0, sizeof(page_select_data_t));

    setup_screen_bg(screen_left);
    setup_screen_bg(screen_right);

    ps.selected_index = find_next(-1, 1);

    create_left_screen(screen_left);
    create_right_screen(screen_right);

    update_left_display();
    update_right_items();

    ESP_LOGI(TAG, "Page select loaded, initial: %s",
             page_list[ps.selected_index].name);
    return 0;
}

static int page_select_update(void)
{
    /* 每 tick（25ms）推进闪烁计时并刷新电池，保证充电闪烁实时进行 */
    ps.blink_ms += APP_UI_TICK_MS;
    page_select_battery_render();
    lv_timer_handler();

    if (ps.needs_refresh) {
        update_left_display();
        update_right_items();
        ps.needs_refresh = false;
        lv_timer_handler();
    }
    return 0;
}

static int page_select_unload(void)
{
    if (ps.left_container) {
        lv_obj_del(ps.left_container);
        ps.left_container = NULL;
    }
    if (ps.right_container) {
        lv_obj_del(ps.right_container);
        ps.right_container = NULL;
    }

    ps.ip_label         = NULL;
    ps.page_name_label  = NULL;
    ps.page_index_label = NULL;
    for (int i = 0; i < 5; i++) ps.bat_segments[i] = NULL;
    ps.bat_tip          = NULL;
    ps.bat_pct_label    = NULL;
    ps.bat_charge_label = NULL;
    ps.hint_label       = NULL;

    for (int i = 0; i < (int)PAGE_COUNT; i++) {
        ps.items[i]       = NULL;
        ps.item_bars[i]   = NULL;
        ps.item_labels[i] = NULL;
    }
    return 0;
}

screen_interface_t page_select_screen_interface = {
    .load         = page_select_load,
    .update       = page_select_update,
    .unload       = page_select_unload,
    .on_key_event = logic_page_page_select_on_key,
};