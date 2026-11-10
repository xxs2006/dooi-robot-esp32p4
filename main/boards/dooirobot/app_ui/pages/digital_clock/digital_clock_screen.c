/* =========================================================================
 * digital_clock_screen.c —— 数码管时钟 / 番茄钟页面
 *
 * 单屏改造后的两个关键问题（原实现踩过）：
 *
 * 1) 逻辑屏只有一个可见
 *    上层仍按 DISPLAY_LEFT / DISPLAY_RIGHT 两个逻辑屏建 root，
 *    但 screen_load_next() 是「先 lv_scr_load(LEFT) 再 lv_scr_load(RIGHT)」，
 *    后加载的 RIGHT 才是真正显示出来的那块。原来「左屏放小时、右屏放分钟」，
 *    结果屏幕上永远只看得见分钟。现在所有绘制统一放到 RIGHT 的 root 里。
 *
 * 2) 7 段字体放不下 HH:MM:SS
 *    ds_digital 是按 200px 转的，单字宽 98px、高 127px，且**没有冒号字形**。
 *    六个数字要 588px，屏只有 320px 宽。
 *    所以这里不再用该字体，改用 LVGL 矩形自己拼 7 段数码管：
 *      · 尺寸完全可控，能塞进 320 宽还能保持数码管观感；
 *      · 每秒只切换 6 位 × 7 段的开/关颜色，不涉及字形渲染，开销极小；
 *      · 冒号自己画两个点。
 *
 * 布局： [时][时] : [分][分] : [秒][秒]
 *        42x96 每字，总宽 304（屏 320），居中显示。
 * ========================================================================= */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include <lvgl.h>

#include "app_ui.h"
#include "app_behavior.h"

static const char *TAG = "clock";

/* ------------------------------------------------------------------ */
/*  7 段数码管几何                                                     */
/* ------------------------------------------------------------------ */
/* 横屏 320x240 的几何：总宽 = 6*42 + 2*12 + 7*4 = 304，塞进 320 还剩 16。
 * 高度 96 在 240 里居中，上下各留 72，数字比竖屏那版大一圈。 */
#define DIG_W       42      /* 单个数字宽 */
#define DIG_H       96      /* 单个数字高 */
#define DIG_T       8       /* 笔画厚度 */
#define DIG_GAP     4       /* 相邻元素间距 */
#define COLON_W     12      /* 冒号占位宽 */
#define DIGIT_N     6       /* 六位：HH MM SS */

/* 段编号：0=A 上横 1=B 右上竖 2=C 右下竖 3=D 下横 4=E 左下竖 5=F 左上竖 6=G 中横 */
#define SEG_A  (1u << 0)
#define SEG_B  (1u << 1)
#define SEG_C  (1u << 2)
#define SEG_D  (1u << 3)
#define SEG_E  (1u << 4)
#define SEG_F  (1u << 5)
#define SEG_G  (1u << 6)

static const uint8_t s_digit_mask[10] = {
    SEG_A|SEG_B|SEG_C|SEG_D|SEG_E|SEG_F,             /* 0 */
    SEG_B|SEG_C,                                     /* 1 */
    SEG_A|SEG_B|SEG_G|SEG_E|SEG_D,                   /* 2 */
    SEG_A|SEG_B|SEG_G|SEG_C|SEG_D,                   /* 3 */
    SEG_F|SEG_G|SEG_B|SEG_C,                         /* 4 */
    SEG_A|SEG_F|SEG_G|SEG_C|SEG_D,                   /* 5 */
    SEG_A|SEG_F|SEG_G|SEG_E|SEG_C|SEG_D,             /* 6 */
    SEG_A|SEG_B|SEG_C,                               /* 7 */
    SEG_A|SEG_B|SEG_C|SEG_D|SEG_E|SEG_F|SEG_G,       /* 8 */
    SEG_A|SEG_B|SEG_C|SEG_D|SEG_F|SEG_G,             /* 9 */
};

/* 只亮中间一横 = 一个短横线，用来表示"时间还没同步/无效" */
#define SEG_DASH        SEG_G

#define COLOR_ON        lv_color_hex(0xFFFFFF)   /* 亮段：纯白 */
#define COLOR_OFF       lv_color_hex(0x0E1114)   /* 灭段：近黑，保留一点数码管轮廓 */

/* ------------------------------------------------------------------ */
/*  页面私有数据                                                       */
/* ------------------------------------------------------------------ */
typedef enum {
    TOMATO_WORK,
    TOMATO_BREAK,
} tomato_state_t;

typedef struct {
    lv_obj_t *box;                   /* 所有图形都挂在它下面，卸载时删一个就够 */
    lv_obj_t *seg[DIGIT_N][7];       /* 6 位数字 × 7 段 */
    uint8_t   mask[DIGIT_N];         /* 当前显示内容，用于避免重复改样式 */
    lv_obj_t *colon[2][2];           /* 2 个冒号 × 2 个点 */
    digital_clock_mode_t current_mode;
    bool is_tomato_running;
    bool ticking_enabled;
    int remaining_seconds;
    tomato_state_t tomato_state;
} digital_clock_data_t;

static digital_clock_data_t clock_data = {
    .box = NULL,
    .current_mode = MODE_TIME,
    .is_tomato_running = true,
    .ticking_enabled = true,
    .remaining_seconds = 25 * 60,
    .tomato_state = TOMATO_WORK
};

/* ------------------------------------------------------------------ */
/*  构建 7 段图形                                                      */
/* ------------------------------------------------------------------ */
static lv_obj_t *seg_rect(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, COLOR_OFF, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, DIG_T / 2, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* 在 (x,y) 处建一个数字的 7 段 */
static void digit_build(lv_obj_t *parent, int idx, int x, int y)
{
    const int h_half = DIG_H / 2;
    const int bar_w  = DIG_W - 2 * (DIG_T - 2);   /* 横段长度，两端各留出竖段宽度 */

    /* A 上横 */
    clock_data.seg[idx][0] = seg_rect(parent, x + DIG_T - 2, y,
                                      bar_w, DIG_T);
    /* B 右上竖 */
    clock_data.seg[idx][1] = seg_rect(parent, x + DIG_W - DIG_T, y + DIG_T - 2,
                                      DIG_T, h_half - DIG_T + 1);
    /* C 右下竖 */
    clock_data.seg[idx][2] = seg_rect(parent, x + DIG_W - DIG_T, y + h_half + 1,
                                      DIG_T, h_half - DIG_T + 1);
    /* D 下横 */
    clock_data.seg[idx][3] = seg_rect(parent, x + DIG_T - 2, y + DIG_H - DIG_T,
                                      bar_w, DIG_T);
    /* E 左下竖 */
    clock_data.seg[idx][4] = seg_rect(parent, x, y + h_half + 1,
                                      DIG_T, h_half - DIG_T + 1);
    /* F 左上竖 */
    clock_data.seg[idx][5] = seg_rect(parent, x, y + DIG_T - 2,
                                      DIG_T, h_half - DIG_T + 1);
    /* G 中横 */
    clock_data.seg[idx][6] = seg_rect(parent, x + DIG_T - 2, y + (DIG_H - DIG_T) / 2,
                                      bar_w, DIG_T);

    clock_data.mask[idx] = 0xFF;     /* 强制首次刷新把全部段点亮/熄灭一次 */
}

static void clock_build(lv_obj_t *parent)
{
    const int total_w = DIGIT_N * DIG_W + 2 * COLON_W + 7 * DIG_GAP;

    /* 屏幕真实分辨率优先。root 尺寸异常时（还没布局完）退回显示器的分辨率，
     * 绝不能让 (area - total)/2 算成负数，否则整块会被裁到屏幕外。 */
    int area_w = lv_obj_get_width(parent);
    int area_h = lv_obj_get_height(parent);
    if (area_w <= 0 || area_h <= 0) {
        lv_display_t *disp = lv_obj_get_display(parent);
        area_w = lv_display_get_horizontal_resolution(disp);
        area_h = lv_display_get_vertical_resolution(disp);
    }
    if (area_w < total_w) area_w = total_w;

    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, total_w, DIG_H);
    lv_obj_set_pos(box, (area_w - total_w) / 2, (area_h - DIG_H) / 2);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    clock_data.box = box;

    /* 元素顺序：d0 d1 : d2 d3 : d4 d5 */
    int x = 0;

    for (int i = 0; i < DIGIT_N; i++) {
        digit_build(box, i, x, 0);
        x += DIG_W + DIG_GAP;

        if (i == 1 || i == 3) {
            /* 两位一组之后插一个冒号 */
            int dot_x = x + (COLON_W - 8) / 2;
            int c     = (i == 1) ? 0 : 1;

            clock_data.colon[c][0] = seg_rect(box, dot_x, DIG_H / 2 - 26, 8, 8);
            clock_data.colon[c][1] = seg_rect(box, dot_x, DIG_H / 2 + 18, 8, 8);
            lv_obj_set_style_bg_color(clock_data.colon[c][0], COLOR_ON, 0);
            lv_obj_set_style_bg_color(clock_data.colon[c][1], COLOR_ON, 0);

            x += COLON_W + DIG_GAP;
        }
    }
}

/* 设置第 idx 位数字要亮的段（只在真的变化时才改样式，省重绘） */
static void clock_set_mask(int idx, uint8_t mask)
{
    if (idx < 0 || idx >= DIGIT_N) return;
    if (clock_data.mask[idx] == mask) return;
    clock_data.mask[idx] = mask;

    for (int s = 0; s < 7; s++) {
        lv_obj_t *o = clock_data.seg[idx][s];
        if (o == NULL) continue;
        bool on = (mask & (1u << s)) != 0;
        lv_obj_set_style_bg_color(o, on ? COLOR_ON : COLOR_OFF, 0);
    }
}

static void clock_show_local_time(void)
{
    time_t now;
    time(&now);
    struct tm *tm = localtime(&now);   /* 必须用 localtime：系统已按 TZ 设好本地时间 */

    if (tm == NULL || tm->tm_year < 2025 - 1900 || tm->tm_sec < 0) {
        for (int i = 0; i < DIGIT_N; i++) clock_set_mask(i, SEG_DASH);
        return;
    }

    int hh = tm->tm_hour, mm = tm->tm_min, ss = tm->tm_sec;
    if (hh > 99) hh = 99;

    clock_set_mask(0, s_digit_mask[hh / 10]);
    clock_set_mask(1, s_digit_mask[hh % 10]);
    clock_set_mask(2, s_digit_mask[mm / 10]);
    clock_set_mask(3, s_digit_mask[mm % 10]);
    clock_set_mask(4, s_digit_mask[ss / 10]);
    clock_set_mask(5, s_digit_mask[ss % 10]);
}

static void clock_show_seconds(int total_seconds)
{
    if (total_seconds < 0) total_seconds = 0;
    if (total_seconds > 99 * 3600 + 59 * 60 + 59) total_seconds = 0;

    int hh = total_seconds / 3600;
    int mm = (total_seconds / 60) % 60;
    int ss = total_seconds % 60;

    clock_set_mask(0, s_digit_mask[hh / 10]);
    clock_set_mask(1, s_digit_mask[hh % 10]);
    clock_set_mask(2, s_digit_mask[mm / 10]);
    clock_set_mask(3, s_digit_mask[mm % 10]);
    clock_set_mask(4, s_digit_mask[ss / 10]);
    clock_set_mask(5, s_digit_mask[ss % 10]);
}

/* ------------------------------------------------------------------ */
/*  对外接口                                                           */
/* ------------------------------------------------------------------ */
void digital_clock_mode_set(int mode)
{
    if (mode >= MODE_TIME && mode < MODE_COUNT) {
        clock_data.current_mode = mode;
    }
}

int digital_clock_mode_get(void)
{
    return clock_data.current_mode;
}

/**
 * @brief 直接设置番茄钟分钟数并重置状态
 * @param mins 分钟数，<=0则使用默认25分钟，上限99分钟
 */
void digital_timer_set_minutes(int mins)
{
    if (mins <= 0) mins = 25;
    if (mins > 99) mins = 99;

    clock_data.remaining_seconds = mins * 60;
    clock_data.is_tomato_running = true;
    clock_data.tomato_state = TOMATO_WORK;
}

void digital_timer_data_set(int mins)
{
    clock_data.remaining_seconds += (mins * 60);

    if (clock_data.remaining_seconds < 0)
        clock_data.remaining_seconds = 0;
    if (clock_data.remaining_seconds >= 100 * 60)
        clock_data.remaining_seconds = 99 * 60;
}

void digital_timer_reset(void)
{
    clock_data.remaining_seconds = 25 * 60;
    clock_data.is_tomato_running = true;
    clock_data.tomato_state = TOMATO_WORK;
}

void digital_clock_mode_change(void)
{
    clock_data.current_mode++;
    if (clock_data.current_mode >= MODE_COUNT) {
        clock_data.current_mode = MODE_TIME;
    }
}

void digital_tomato_toggle(void)
{
    clock_data.is_tomato_running = !clock_data.is_tomato_running;
}

void digital_clock_ticking_toggle(void)
{
    clock_data.ticking_enabled = !clock_data.ticking_enabled;
}

/* ------------------------------------------------------------------ */
/*  每秒刷新                                                           */
/* ------------------------------------------------------------------ */
static void update_display(void)
{
    if (clock_data.current_mode == MODE_TIME) {
        clock_show_local_time();
        return;
    }

    /* MODE_TIMER：番茄钟倒计时 */
    clock_show_seconds(clock_data.remaining_seconds);

    if (!clock_data.is_tomato_running) return;

    if (clock_data.remaining_seconds > 0) {
        clock_data.remaining_seconds--;
        if (clock_data.remaining_seconds == 0) {
            if (clock_data.tomato_state == TOMATO_WORK) {
                ApplicationSendMessage("番茄钟工作时间结束，提醒用户该休息了；表达要活泼可爱；比如：叮叮叮！25分钟专注时间结束啦，快起来伸个懒腰，休息5分钟吧～", true);
            } else {
                ApplicationSendMessage("番茄钟休息时间结束，提醒用户继续工作；表达要鼓励加油；比如：休息时间到！充电完毕，让我们继续加油，再来一个25分钟的专注吧！", true);
            }
        }
    } else {
        clock_data.tomato_state = (clock_data.tomato_state == TOMATO_WORK) ? TOMATO_BREAK : TOMATO_WORK;

        if (clock_data.tomato_state == TOMATO_WORK) {
            clock_data.remaining_seconds = 25 * 60;
        } else {
            clock_data.remaining_seconds = 5 * 60;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  页面生命周期                                                       */
/* ------------------------------------------------------------------ */
static int digital_clock_load(void)
{
    /* 只往 RIGHT 建：两个逻辑屏共用一块物理屏，后 load 的 RIGHT 才是可见的 */
    lv_obj_t *root = screen_manager.screens[DISPLAY_RIGHT].root;
    if (root == NULL) {
        ESP_LOGE(TAG, "RIGHT screen root is NULL, clock skipped");
        return -1;
    }

    if (clock_data.box != NULL) {      /* 防止重复 load 叠对象 */
        lv_obj_del(clock_data.box);
        clock_data.box = NULL;
    }
    memset(clock_data.seg,   0, sizeof(clock_data.seg));
    memset(clock_data.colon, 0, sizeof(clock_data.colon));
    memset(clock_data.mask,  0, sizeof(clock_data.mask));

    clock_build(root);

    /* 立刻画一次，避免进页面先看到六个 8 */
    update_display();
    lv_timer_handler();

    ESP_LOGI(TAG, "clock page loaded (%dx%d digits)", DIG_W, DIG_H);
    return 0;
}

static int digital_clock_update(void)
{
    static int tick_counter = 0;

    if (tick_counter++ >= APP_UI_MS_TO_TICKS(1000)) {
        tick_counter = 0;
        update_display();
        lv_timer_handler();

        if (clock_data.current_mode == MODE_TIMER && clock_data.ticking_enabled) {
            app_behavior_play_scene("tomato_tick", NULL, BEHAVIOR_PRIO_DEFAULT);
        }
    }

    return 0;
}

static int digital_clock_unload(void)
{
    /* 删 box 会连带删掉下面所有段和冒号 */
    if (clock_data.box != NULL) {
        lv_obj_del(clock_data.box);
        clock_data.box = NULL;
    }
    memset(clock_data.seg,   0, sizeof(clock_data.seg));
    memset(clock_data.colon, 0, sizeof(clock_data.colon));
    memset(clock_data.mask,  0, sizeof(clock_data.mask));

    return 0;
}

screen_interface_t digital_clock_screen_interface = {
    .load = digital_clock_load,
    .update = digital_clock_update,
    .unload = digital_clock_unload,
    .on_key_event = logic_page_digital_clock_on_key,
};
