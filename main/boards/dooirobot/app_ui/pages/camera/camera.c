#include <lvgl.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "app_ui.h"
#include "display_hal.h"
#include "dooi_camera_iface.h"
#include "app_vision.h"
#include "app_ui_logic.h"
#include "face_tracker.h"   // ← 新增

static const char *TAG = "camera_screen";

LV_FONT_DECLARE(lv_font_notosans_cs_medium_14);

//====================================================
// 显示配置
//====================================================
#define CAMERA_INPUT_WIDTH          200
#define CAMERA_INPUT_HEIGHT         200
#define MAX_FACES                   1
#define PHOTO_FREEZE_TIMEOUT_FRAMES 300     // 10秒 @30fps

//====================================================
// 持久化状态
//====================================================
static camera_mode_t    s_saved_mode    = CAMERA_MODE_PREVIEW;
static camera_trigger_t s_saved_trigger = CAMERA_TRIGGER_NONE;

/* 显示缓冲跨页面生命周期复用（camera_screen 每次 load/unload 都会 memset，
   放在模块静态变量里，避免每次进出页面都重新分配 / 泄漏一块 PSRAM） */
static uint8_t         *s_display_buf   = NULL;

//====================================================
// 外部跳转请求
//====================================================
static camera_mode_t    s_initial_mode     = CAMERA_MODE_PREVIEW;
static bool             s_initial_mode_set = false;
static camera_trigger_t s_pending_trigger  = CAMERA_TRIGGER_NONE;

//====================================================
// 模式描述表
//====================================================
typedef struct {
    const char    *name_cn;
    const char    *name_en;
    vision_mode_t  model;
    const char    *hint;
} mode_info_t;

static const mode_info_t mode_infos[CAMERA_MODE_MAX] = {
    [CAMERA_MODE_NULL]              = { "空模式",   "NULL",        VISION_MODE_NONE,              "" },
    [CAMERA_MODE_PREVIEW]           = { "预览模式", "PREVIEW",     VISION_MODE_NONE,              "L:Mode R:Next\nDouble:Back" },
    [CAMERA_MODE_PHOTO]             = { "快照模式", "PHOTO",       VISION_MODE_NONE,              "L:Capture\nR:Next Mode" },
    [CAMERA_MODE_FACE_DETECT]       = { "人脸识别", "FACE DETECT", VISION_MODE_FACE_DETECT,       "L:Mode R:Next\nAuto Detect" },
    [CAMERA_MODE_GESTURE_RECOGNIZE] = { "手势识别", "GESTURE",     VISION_MODE_GESTURE_RECOGNIZE, "L:Mode R:Next\nAuto Detect" },
};

//====================================================
// 人脸框
//====================================================
typedef struct {
    lv_obj_t *box_obj;
    bool       active;
    int        x, y, w, h;
} face_box_t;

//====================================================
// 主数据结构
//====================================================
typedef struct {
    // 左屏 UI
    lv_obj_t *left_container;
    lv_obj_t *left_title_label;
    lv_obj_t *left_mode_label;
    lv_obj_t *left_status_label;
    lv_obj_t *left_index_label;
    lv_obj_t *left_hint_label;

    // 右屏 UI
    lv_obj_t    *right_container;
    lv_obj_t    *right_obj;
    lv_img_dsc_t img_dsc;
    uint8_t     *display_buf;

    // 人脸框
    face_box_t face_boxes[MAX_FACES];

    // 模式管理
    camera_mode_t current_mode;
    camera_mode_t pending_mode;
    bool          mode_switch_requested;

    // 快照
    bool     freeze_frame;
    uint32_t freeze_frame_counter;

    // 识别结果缓存（手势 / 通用状态）
    char   status_text[64];
    bool   status_updated;

    // 人脸结果
    vision_result_t face_results[MAX_FACES];
    size_t          face_count;
    bool            face_updated;
    uint32_t        last_face_time;

    bool unloading;
} camera_screen_t;

static camera_screen_t camera_screen = {0};



//====================================================
// 坐标映射
//====================================================
/* 显示画面被 PPA 顺时针转了 90°（见 dooi_camera.cc 里 display 那一路的
 * PPA_SRM_ROTATION_ANGLE_90），但人脸检测跑的是**未旋转**的 200x200 原图。
 * 所以框坐标必须跟着一起转，否则框会固定在错误的位置。
 *
 * 顺时针 90°、边长 N 的映射：
 *   原图左上 (mx, my) + 尺寸 (mw, mh)
 *     -> 新图 (N - (my + mh), mx)，宽高互换 (mh, mw)
 * 验证：原图左上角的小框 -> 新图右上角，符合顺时针直觉。 */
static inline void map_model_to_display(int mx, int my, int mw, int mh,
                                        int *dx, int *dy, int *dw, int *dh)
{
    const int N = CAMERA_INPUT_WIDTH;

    int x = N - (my + mh);
    int y = mx;
    int w = mh;
    int h = mw;

    if (x < 0)   x = 0;
    if (y < 0)   y = 0;
    if (x + w > N) w = N - x;
    if (y + h > N) h = N - y;

    *dx = x; *dy = y; *dw = w; *dh = h;
}

//====================================================
// face_tracker 回调实现
//====================================================

/**
 * @brief 向追踪器提供最新人脸检测结果
 *        追踪器每帧调用一次，我们直接返回 camera_screen 的当前帧数据
 */
static bool ft_get_result(vision_result_t *out_result, void *user_data)
{
    (void)user_data;
    if (camera_screen.face_count > 0) {
        memcpy(out_result, &camera_screen.face_results[0], sizeof(vision_result_t));
        return true;
    }
    return false;
}

/** @brief 坐标映射回调，转发给内部 map_model_to_display */
static void ft_map_display(int mx,  int my,  int mw,  int mh,
                           int *dx, int *dy, int *dw, int *dh,
                           void *user_data)
{
    (void)user_data;
    map_model_to_display(mx, my, mw, mh, dx, dy, dw, dh);
}

/** @brief 追踪器需要更新状态文字时调用 */
static void ft_on_status(const char *text, void *user_data)
{
    (void)user_data;
    if (camera_screen.unloading || !camera_screen.left_status_label || !text) return;
    lv_label_set_text(camera_screen.left_status_label, text);
}

/** @brief 单次对齐完成回调（目前无额外 UI 动作，事件已在追踪器内部发出） */
static void ft_on_aligned(void *user_data)
{
    (void)user_data;
    // 预留：可在此触发快照、动画等
}

//====================================================
// 人脸框管理
//====================================================
static void create_face_box(int index)
{
    if (index >= MAX_FACES || camera_screen.face_boxes[index].box_obj) return;
    if (camera_screen.unloading || !camera_screen.right_container) return;

    lv_obj_t *box = lv_obj_create(camera_screen.right_container);
    lv_obj_remove_style_all(box);
    lv_obj_set_style_border_width(box, 3, 0);
    lv_obj_set_style_border_color(box, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    camera_screen.face_boxes[index].box_obj = box;
    camera_screen.face_boxes[index].active  = false;
}

static void update_face_box(int index, int x, int y, int w, int h, bool visible)
{
    if (index >= MAX_FACES) return;
    face_box_t *b = &camera_screen.face_boxes[index];
    if (!b->box_obj) create_face_box(index);
    if (!b->box_obj) return;

    if (visible) {
        lv_obj_set_pos(b->box_obj, x, y);
        lv_obj_set_size(b->box_obj, w, h);
        lv_obj_clear_flag(b->box_obj, LV_OBJ_FLAG_HIDDEN);
        b->active = true; b->x = x; b->y = y; b->w = w; b->h = h;
    } else {
        lv_obj_add_flag(b->box_obj, LV_OBJ_FLAG_HIDDEN);
        b->active = false;
    }
}

static void hide_all_face_boxes(void)
{
    for (int i = 0; i < MAX_FACES; i++) {
        if (camera_screen.face_boxes[i].box_obj) {
            lv_obj_add_flag(camera_screen.face_boxes[i].box_obj, LV_OBJ_FLAG_HIDDEN);
            camera_screen.face_boxes[i].active = false;
        }
    }
}

//====================================================
// 左屏信息
//====================================================
static void update_left_info(void)
{
    if (camera_screen.unloading || camera_screen.current_mode >= CAMERA_MODE_MAX) return;
    const mode_info_t *info = &mode_infos[camera_screen.current_mode];

    if (camera_screen.left_mode_label)
        lv_label_set_text(camera_screen.left_mode_label, info->name_cn);

    if (camera_screen.left_index_label) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d/%d", camera_screen.current_mode, CAMERA_MODE_MAX - 1);
        lv_label_set_text(camera_screen.left_index_label, buf);
    }

    if (camera_screen.left_hint_label)
        lv_label_set_text(camera_screen.left_hint_label, info->hint);
}

static void update_status_text(const char *text)
{
    if (camera_screen.unloading || !camera_screen.left_status_label || !text) return;
    lv_label_set_text(camera_screen.left_status_label, text);
}

//====================================================
// 模式切换
//====================================================
void camera_switch_mode_internal(camera_mode_t new_mode)
{
    if (camera_screen.unloading || new_mode >= CAMERA_MODE_MAX) return;
    if (camera_screen.current_mode == new_mode) return;

    if (camera_screen.current_mode == CAMERA_MODE_FACE_DETECT)
        face_tracker_enable(false, false);

    const mode_info_t *info = &mode_infos[new_mode];
    ESP_LOGI(TAG, "Switching to mode: %s", info->name_en);

    if (info->model != VISION_MODE_NONE) {
        app_event_t e = {
            .type = APP_EVT_VISION_TASK_STARTED,
            .data.vision_task_data = {
                .mode = info->model,
                .intervals = { .face_detect_ms = 10, .gesture_recognize_ms = 10 }
            }
        };
        app_ui_logic_post(&e, false);
    } else {
        app_event_t e = { .type = APP_EVT_VISION_TASK_EXITED };
        app_ui_logic_post(&e, false);
    }

    if (new_mode != CAMERA_MODE_FACE_DETECT) hide_all_face_boxes();

    camera_screen.status_text[0]       = '\0';
    camera_screen.face_count           = 0;
    camera_screen.status_updated       = false;
    camera_screen.face_updated         = false;
    camera_screen.last_face_time       = lv_tick_get();
    camera_screen.freeze_frame         = false;
    camera_screen.freeze_frame_counter = 0;

    camera_screen.current_mode = new_mode;
    update_left_info();

    switch (new_mode) {
        case CAMERA_MODE_PHOTO:             update_status_text("准备拍照");      break;
        case CAMERA_MODE_PREVIEW:           update_status_text("实时预览中..."); break;
        case CAMERA_MODE_FACE_DETECT:       update_status_text("等待检测...");   break;
        case CAMERA_MODE_GESTURE_RECOGNIZE: update_status_text("等待识别...");   break;
        default: break;
    }

    if (new_mode == CAMERA_MODE_FACE_DETECT && s_pending_trigger == CAMERA_TRIGGER_NONE)
        s_pending_trigger = CAMERA_TRIGGER_FACE_TRACK_LOOP;
}

//====================================================
// 视觉结果回调
//====================================================
void camera_on_vision_result_to_screen(vision_mode_t mode,
                                       const vision_result_t *results,
                                       size_t count, void *user_data)
{
    (void)user_data;
    if (camera_screen.unloading) return;

    switch (mode) {
                case VISION_MODE_FACE_DETECT:
            if (count > 0) {
                camera_screen.last_face_time = lv_tick_get();
                camera_screen.face_count = (count > MAX_FACES) ? MAX_FACES : count;
                for (size_t i = 0; i < camera_screen.face_count; i++)
                    memcpy(&camera_screen.face_results[i], &results[i], sizeof(vision_result_t));
                camera_screen.face_updated = true;
            }
            break;

        case VISION_MODE_GESTURE_RECOGNIZE:
            if (count > 0 && results[0].label) {
                snprintf(camera_screen.status_text, sizeof(camera_screen.status_text),
                         "手势: %s\n置信度: %.0f%%",
                         results[0].label, results[0].score * 100.0f);
                camera_screen.status_updated = true;
            }
            break;

        default:
            break;
    }
}

//====================================================
// 各模式结果处理（camera_update 调用）
//====================================================
static void process_face_results(void)
{
    if (!camera_screen.face_updated) return;
    camera_screen.face_updated = false;

    snprintf(camera_screen.status_text, sizeof(camera_screen.status_text),
             camera_screen.face_count > 0 ? "检测到 %d 张人脸" : "未检测到人脸",
             (int)camera_screen.face_count);
    update_status_text(camera_screen.status_text);

    for (size_t i = 0; i < MAX_FACES; i++) {
        if (i < camera_screen.face_count) {
            const vision_result_t *r = &camera_screen.face_results[i];
            int dx, dy, dw, dh;
            map_model_to_display(r->box[0], r->box[1],
                                 r->box[2] - r->box[0], r->box[3] - r->box[1],
                                 &dx, &dy, &dw, &dh);
            update_face_box(i, dx, dy, dw, dh, true);
        } else {
            update_face_box(i, 0, 0, 0, 0, false);
        }
    }
}

static void process_gesture_results(void)
{
    if (!camera_screen.status_updated) return;
    camera_screen.status_updated = false;
    update_status_text(camera_screen.status_text);
}

//====================================================
// 左屏 UI 创建
//====================================================
static void create_left_screen(lv_obj_t *parent)
{
    if (!parent) { ESP_LOGE(TAG, "Left parent is NULL"); return; }

    camera_screen.left_container = lv_obj_create(parent);
    lv_obj_remove_style_all(camera_screen.left_container);
    lv_obj_set_size(camera_screen.left_container, 200, 200);
    lv_obj_set_style_radius(camera_screen.left_container, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(camera_screen.left_container, 2, 0);
    lv_obj_set_style_border_color(camera_screen.left_container, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_center(camera_screen.left_container);
    lv_obj_clear_flag(camera_screen.left_container, LV_OBJ_FLAG_SCROLLABLE);

    camera_screen.left_title_label = lv_label_create(camera_screen.left_container);
    lv_label_set_text(camera_screen.left_title_label, "CAMERA");
    lv_obj_set_style_text_color(camera_screen.left_title_label, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_font(camera_screen.left_title_label, &lv_font_montserrat_14, 0);
    lv_obj_align(camera_screen.left_title_label, LV_ALIGN_CENTER, 0, -70);

    camera_screen.left_mode_label = lv_label_create(camera_screen.left_container);
    lv_label_set_text(camera_screen.left_mode_label, "");
    lv_obj_set_style_text_color(camera_screen.left_mode_label, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_text_font(camera_screen.left_mode_label, &lv_font_notosans_cs_medium_14, 0);
    lv_obj_set_width(camera_screen.left_mode_label, 160);
    lv_obj_set_style_text_align(camera_screen.left_mode_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(camera_screen.left_mode_label, LV_ALIGN_CENTER, 0, -35);

    camera_screen.left_status_label = lv_label_create(camera_screen.left_container);
    lv_label_set_text(camera_screen.left_status_label, "");
    lv_obj_set_style_text_color(camera_screen.left_status_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(camera_screen.left_status_label, &lv_font_notosans_cs_medium_14, 0);
    lv_obj_set_width(camera_screen.left_status_label, 160);
    lv_obj_set_style_text_align(camera_screen.left_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(camera_screen.left_status_label, LV_ALIGN_CENTER, 0, 0);

    camera_screen.left_index_label = lv_label_create(camera_screen.left_container);
    lv_label_set_text(camera_screen.left_index_label, "1/4");
    lv_obj_set_style_text_color(camera_screen.left_index_label, lv_color_hex(0xA0A0A0), 0);
    lv_obj_set_style_text_font(camera_screen.left_index_label, &lv_font_montserrat_14, 0);
    lv_obj_align(camera_screen.left_index_label, LV_ALIGN_CENTER, 0, 30);

    camera_screen.left_hint_label = lv_label_create(camera_screen.left_container);
    lv_label_set_text(camera_screen.left_hint_label, "");
    lv_obj_set_style_text_color(camera_screen.left_hint_label, lv_color_hex(0x606060), 0);
    lv_obj_set_style_text_font(camera_screen.left_hint_label, &lv_font_notosans_cs_medium_14, 0);
    lv_obj_set_style_text_align(camera_screen.left_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(camera_screen.left_hint_label, 160);
    lv_obj_align(camera_screen.left_hint_label, LV_ALIGN_CENTER, 0, 65);
}

//====================================================
// 生命周期
//====================================================
int camera_load(void)
{
    ESP_LOGI(TAG, "Loading camera screen...");

    lv_obj_t *screen_left  = screen_manager.screens[DISPLAY_LEFT].root;
    lv_obj_t *screen_right = screen_manager.screens[DISPLAY_RIGHT].root;
    if (!screen_left || !screen_right) { ESP_LOGE(TAG, "Screen root NULL"); return -1; }

    memset(&camera_screen, 0, sizeof(camera_screen));

    // ── 初始化追踪器（注入回调）────────────────────────────────────
    face_tracker_config_t ft_cfg = {
        .get_result  = ft_get_result,
        .map_display = ft_map_display,
        .on_status   = ft_on_status,
        .on_aligned  = ft_on_aligned,
        .user_data   = NULL,
    };
    face_tracker_init(&ft_cfg);

    size_t buf_size = CAMERA_INPUT_WIDTH * CAMERA_INPUT_HEIGHT * sizeof(uint16_t);
    if (!s_display_buf) {
        s_display_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_display_buf) { ESP_LOGE(TAG, "display_buf malloc failed"); return -1; }
    }
    camera_screen.display_buf = s_display_buf;
    memset(camera_screen.display_buf, 0, buf_size);

    camera_screen.img_dsc.header.cf   = LV_COLOR_FORMAT_RGB565;
    camera_screen.img_dsc.header.w    = CAMERA_INPUT_WIDTH;
    camera_screen.img_dsc.header.h    = CAMERA_INPUT_HEIGHT;
    camera_screen.img_dsc.data        = camera_screen.display_buf;
    camera_screen.img_dsc.data_size   = buf_size;

    // 右屏圆形容器
    camera_screen.right_container = lv_obj_create(screen_right);
    lv_obj_remove_style_all(camera_screen.right_container);
    lv_obj_set_size(camera_screen.right_container, CAMERA_INPUT_WIDTH, CAMERA_INPUT_HEIGHT);
    lv_obj_set_style_radius(camera_screen.right_container, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(camera_screen.right_container, true, 0);
    lv_obj_set_style_border_width(camera_screen.right_container, 2, 0);
    lv_obj_set_style_border_color(camera_screen.right_container, lv_color_hex(0x404040), 0);
    lv_obj_center(camera_screen.right_container);
    lv_obj_clear_flag(camera_screen.right_container, LV_OBJ_FLAG_SCROLLABLE);

    camera_screen.right_obj = lv_img_create(camera_screen.right_container);
    lv_obj_set_size(camera_screen.right_obj, CAMERA_INPUT_WIDTH, CAMERA_INPUT_HEIGHT);
    lv_obj_center(camera_screen.right_obj);
    lv_img_set_src(camera_screen.right_obj, &camera_screen.img_dsc);

    create_left_screen(screen_left);

    camera_screen.current_mode          = CAMERA_MODE_MAX;
    camera_screen.pending_mode          = s_initial_mode_set ? s_initial_mode : s_saved_mode;
    camera_screen.mode_switch_requested = true;

    if (s_pending_trigger == CAMERA_TRIGGER_NONE)
        s_pending_trigger = s_saved_trigger;

    s_initial_mode_set = false;
    s_initial_mode     = CAMERA_MODE_PREVIEW;

    ESP_LOGI(TAG, "Camera screen loaded, pending mode: %s",
             mode_infos[camera_screen.pending_mode].name_en);
    return 0;
}

static int camera_update(void)
{
    // 1. 模式切换
    if (camera_screen.mode_switch_requested) {
        camera_screen.mode_switch_requested = false;
        camera_switch_mode_internal(camera_screen.pending_mode);

        if (s_pending_trigger != CAMERA_TRIGGER_NONE) {
            camera_trigger_t trig = s_pending_trigger;
            s_pending_trigger = CAMERA_TRIGGER_NONE;
            switch (trig) {
                case CAMERA_TRIGGER_FACE_ALIGN_ONCE:
                    face_tracker_enable(true, false);
                    break;
                case CAMERA_TRIGGER_FACE_TRACK_LOOP:
                    face_tracker_enable(true, true);
                    break;
                default:
                    break;
            }
        }
    }

    // 2. 快照超时
    if (camera_screen.current_mode == CAMERA_MODE_PHOTO && camera_screen.freeze_frame) {
        camera_screen.freeze_frame_counter++;
        uint32_t rem = PHOTO_FREEZE_TIMEOUT_FRAMES - camera_screen.freeze_frame_counter;
        if (camera_screen.freeze_frame_counter % 30 == 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d秒返回", (int)((rem + 29) / 30));
            update_status_text(buf);
        }
        if (camera_screen.freeze_frame_counter >= PHOTO_FREEZE_TIMEOUT_FRAMES)
            screen_set_current(screen_manager.old_screen);
    }

    // 3. 识别结果处理
    switch (camera_screen.current_mode) {
        case CAMERA_MODE_FACE_DETECT:
            // 超过 1 秒无新帧，清零人脸计数
            if (camera_screen.face_count > 0 &&
                (lv_tick_get() - camera_screen.last_face_time > 1000)) {
                camera_screen.face_count   = 0;
                camera_screen.face_updated = true;
            }
            process_face_results();
            break;

        case CAMERA_MODE_GESTURE_RECOGNIZE:
            process_gesture_results();
            break;

        default:
            break;
    }

    // 4. 显示帧
    if (!camera_screen.freeze_frame) {
        FrameBuffer frame = camera_get_display_frame();
        if (frame.data && frame.len > 0) {
            size_t data_size = frame.width * frame.height * 2;
            memcpy(camera_screen.display_buf, frame.data, data_size);
            camera_screen.img_dsc.header.w  = frame.width;
            camera_screen.img_dsc.header.h  = frame.height;
            camera_screen.img_dsc.data_size = data_size;
        }
        if (camera_screen.right_obj) {
            lv_img_set_src(camera_screen.right_obj, &camera_screen.img_dsc);
            lv_obj_invalidate(camera_screen.right_obj);
        }
        if (camera_screen.current_mode == CAMERA_MODE_PHOTO) {
            camera_screen.freeze_frame         = true;
            camera_screen.freeze_frame_counter = 0;
            update_status_text("快照");
            ESP_LOGI(TAG, "Frame frozen");
        }
    }

    lv_timer_handler();
    return 0;
}

int camera_unload(void)
{
    ESP_LOGI(TAG, "Unloading camera screen...");
    camera_screen.unloading = true;

    // 保存当前模式和触发器状态
    if (camera_screen.current_mode < CAMERA_MODE_MAX) {
        s_saved_mode = camera_screen.current_mode;
        if (camera_screen.current_mode == CAMERA_MODE_FACE_DETECT && face_tracker_is_enabled())
            s_saved_trigger = CAMERA_TRIGGER_FACE_TRACK_LOOP;
        else
            s_saved_trigger = CAMERA_TRIGGER_NONE;
    }

    /* 关键：必须先解绑追踪器回调再销毁对象。
       screen_load() 在 unload() 之后会调用 lv_timer_handler()，
       lv_obj_del_async 会在此刻真正释放 label；而 face_tracker_update()
       紧接着在同一轮 while 循环里被调用（app_ui.c:185）。
       若此时 on_status 仍指向 ft_on_status，就会对已释放的 label
       调用 lv_label_set_text() 而崩溃。
       用 unbind() 而不是 deinit()：只清回调、保留 enabled，
       这样跨页面追踪（face_tracker_push_result）仍能继续工作。 */
    face_tracker_unbind();

    app_event_t evt = {
        .type = APP_EVT_VISION_TASK_STARTED,
        .data.vision_task_data = {
            .mode = VISION_MODE_ROUND_ROBIN,
            .intervals = {
                .face_detect_ms       = 10,
                .gesture_recognize_ms = 10,
            }
        }
    };
    app_ui_logic_post(&evt, false);

    if (camera_screen.right_container) {
        lv_obj_del_async(camera_screen.right_container);
    }
    if (camera_screen.left_container) {
        lv_obj_del_async(camera_screen.left_container);
    }

    /* 清空所有对象指针（含 left_status_label 等），避免悬空引用。
       display_buf 保留给下次 load 复用，不在此释放。 */
    memset(&camera_screen, 0, sizeof(camera_screen));
    camera_screen.unloading = true;   /* 由 camera_load() 的 memset 复位 */

    ESP_LOGI(TAG, "Camera screen unloaded (saved mode=%s)", mode_infos[s_saved_mode].name_en);
    return 0;
}

//====================================================
// 按键事件
//====================================================
void camera_mode_switch(camera_mode_t new_mode)
{
    if (new_mode >= CAMERA_MODE_MAX) return;
    camera_screen.pending_mode          = new_mode;
    camera_screen.mode_switch_requested = true;
}

camera_mode_t camera_mode_get(void)
{
    return camera_screen.current_mode;
}

//====================================================
// 导出接口
//====================================================
screen_interface_t camera_screen_interface = {
    .load         = camera_load,
    .update       = camera_update,
    .unload       = camera_unload,
    .on_key_event = logic_page_camera_on_key,
};

//====================================================
// 外部跳转入口
//====================================================
void camera_trigger_funtion(const camera_request_t *req)
{
    if (!req) {
        s_initial_mode     = CAMERA_MODE_PREVIEW;
        s_initial_mode_set = false;
        s_pending_trigger  = CAMERA_TRIGGER_NONE;
    } else {
        camera_mode_t mode = req->mode;
        if (mode <= CAMERA_MODE_NULL || mode >= CAMERA_MODE_MAX)
            mode = CAMERA_MODE_PREVIEW;
        s_initial_mode     = mode;
        s_initial_mode_set = true;
        s_pending_trigger  = req->trigger;
    }
    screen_set_current(SCREEN_CAMERA);
}

void camera_trigger_face_align_once(void)
{
    ESP_LOGI(TAG, "External Trigger: Face Align Once");
    if (camera_screen.current_mode != CAMERA_MODE_FACE_DETECT)
        camera_mode_switch(CAMERA_MODE_FACE_DETECT);
    else
        face_tracker_enable(true, false);
}

void camera_trigger_face_tracking_loop(void)
{
    ESP_LOGI(TAG, "External Trigger: Continuous Tracking");
    if (camera_screen.current_mode != CAMERA_MODE_FACE_DETECT)
        camera_mode_switch(CAMERA_MODE_FACE_DETECT);
    else
        face_tracker_enable(true, true);
}