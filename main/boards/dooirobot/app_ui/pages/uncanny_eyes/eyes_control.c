// eyes_control.c
#include "eyes_control.h"
#include "eye_source.h"
#include "eyes_config.h"
#include "eyes_port.h"
#include "eyes_port_disp_hal.h"
#include "display_hal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "app_ui.h"
#include "dooi_nvs.h"
#include "app_servo.h"
#include "app_led.h"
#include "app_behavior.h"
#include "app_ui_logic.h"
#include "logic_emotion.h"
#include "dooi_behavior.h"

static TaskHandle_t eyes_task1 = NULL;
static TaskHandle_t eyes_task2 = NULL;

static EventGroupHandle_t s_eye_events  = NULL;
static volatile bool      s_eyes_running = false;

/* 置 1 打开每秒一次的眼睛渲染性能日志 */
#ifndef EYES_PERF_LOG
#define EYES_PERF_LOG 1
#endif

#define EYE0_STOPPED_BIT  (1 << 0)
#define EYE1_STOPPED_BIT  (1 << 1)
#define EYES_ALL_STOPPED  (EYE0_STOPPED_BIT | EYE1_STOPPED_BIT)

/* ================================================================
 *                     情 绪 自 动 更 新
 *  注意：情绪状态机已迁入 ui_logic (logic_emotion.c)，
 *        以下为保持 API 兼容的薄转发层。
 * ================================================================ */
void eye_controller_set_emotion_auto(EYES_EMOTION emotion)
{
    logic_emotion_set_auto(emotion);
}

void eye_controller_apply_emotion(EYES_EMOTION emotion)
{
    logic_emotion_set_manual(emotion);
}

bool eye_controller_is_manual_emotion(void)
{
    return logic_emotion_is_manual();
}

/* ================================================================
 *              运 行 时 直 调（网页 / 外部工具用）
 * ================================================================ */
void eyes_control_apply_preset(const EyeExpressionPreset *preset)
{
    if (!preset) return;
    logic_emotion_hold_custom();          /* 先推迟自动随机切换 */
    eyes_enter_critical();
    eye_controller_apply_preset(preset);
    eyes_exit_critical();
}

void eyes_control_set_lid(uint32_t eye_index, EYES_LIDS_DEFINE lid_type,
                          float upper, float lower, uint32_t duration_ms)
{
    if (eye_index > 1) return;
    logic_emotion_hold_custom();
    eyes_enter_critical();
    eye_controller_set_lid(eye_index, lid_type, upper, lower, duration_ms);
    eyes_exit_critical();
}

void eyes_control_gaze(uint32_t eye_index, float x, float y, int32_t duration_ms)
{
    if (eye_index > 1) return;
    logic_emotion_hold_custom();
    eyes_enter_critical();
    eye_controller_set_target_position_eye(eye_index, x, y, duration_ms);
    eyes_exit_critical();
}

void eyes_control_pupil(uint32_t eye_index, float ratio, int32_t duration_ms)
{
    if (eye_index > 1) return;
    logic_emotion_hold_custom();
    eyes_enter_critical();
    eye_controller_set_target_pupil_eye(eye_index, ratio, duration_ms);
    eyes_exit_critical();
}

void eyes_control_scale(uint32_t eye_index, float ratio, int32_t duration_ms)
{
    if (eye_index > 1) return;
    logic_emotion_hold_custom();
    eyes_enter_critical();
    eye_controller_set_target_scale_eye(eye_index, ratio, duration_ms);
    eyes_exit_critical();
}

void eyes_control_blink(uint32_t duration)
{
    eyes_enter_critical();
    eye_controller_blink(duration);
    eyes_exit_critical();
}

void eyes_control_wink(uint32_t eye_index)
{
    if (eye_index > 1) return;
    eyes_enter_critical();
    eye_controller_wink(eye_index);
    eyes_exit_critical();
}

/* ================================================================
 *                     屏 幕 生 命 周 期
 * ================================================================ */
int eyes_screen_load(void)
{
    EyeController *control = eye_controller_get_instance();

    logic_page_eyes_reset_state();

    xEventGroupClearBits(s_eye_events, EYES_ALL_STOPPED);
    s_eyes_running = true;

    /* 单屏双眼改造：
     * 1) 不再改面板镜像 —— 初始化时已按 DISP_MIRROR_X/Y 校准好 MADCTL(=0xC0)，
     *    这里再改会整屏转 180°，眼睛就反了。两块逻辑屏合成一块物理屏后，
     *    也不存在「左右眼各自镜像」的概念。
     * 2) 进页面前先清屏：擦掉上一页残留的 LVGL 像素，同时给眼睛带铺黑底
     *    （眼睛只覆盖中间 240x120 那条，上下必须自己刷黑）。 */
    display_clear_all(0x0000);

#if DISP_PROBE_CORNERS
    /* 一次性确认坐标映射：四个标记应分别贴在玻璃的四个角 */
    display_draw_probe_corners();
#endif

    memset(&control->eyes[0].lastRenderState, 0, sizeof(EyeRenderState));
    memset(&control->eyes[1].lastRenderState, 0, sizeof(EyeRenderState));

    /* 脏检测缓存失效：重新进页面必须整屏重绘一次 */
    control->eyes[0].dirtyKey.valid = 0;
    control->eyes[1].dirtyKey.valid = 0;
    control->eyes[0].drawAll = true;
    control->eyes[1].drawAll = true;

    dooi_behavior_enable(true);

    vTaskResume(eyes_task1);
    vTaskResume(eyes_task2);

    return 0;
}

int eyes_screen_unload(void)
{
    dooi_behavior_enable(false);
    s_eyes_running = false;

    EventBits_t bits = xEventGroupWaitBits(
        s_eye_events,
        EYES_ALL_STOPPED,
        pdTRUE,
        pdTRUE,
        pdMS_TO_TICKS(2000)
    );

    if ((bits & EYES_ALL_STOPPED) != EYES_ALL_STOPPED) {
        vTaskSuspend(eyes_task1);
        vTaskSuspend(eyes_task2);
    }

    /* 单屏：整屏清黑（原来按 NUM_DISPLAYS 循环，单屏下等价，统一走 display_clear_all） */
    display_clear_all(0x0000);

    return 0;
}

screen_interface_t eyes_screen_interface =
{
    .load        = eyes_screen_load,
    .update      = NULL,
    .unload      = eyes_screen_unload,
    .on_key_event = logic_page_eyes_on_key,
};

/* ================================================================
 *                     渲 染 初 始 化 & 任 务
 * ================================================================ */
int eyes_render_init(void)
{
    /* 先把跨平台库接到本工程的 display_hal（库本身不认识任何硬件） */
    eyes_port_disp_hal_install();

    eye_controller_init(&bigBlue, &bigBlue);
    eye_controller_apply_emotion(EYES_EMOTION_FOCUS_BELOW_CENTER);
    uint8_t iris = 7;
    dooi_nvs_get_eye_iris(&iris);
    eye_controller_apply_iris_texture(iris);

    s_eye_events = xEventGroupCreate();
    assert(s_eye_events);

    xTaskCreatePinnedToCore(eyes_render_task, "eye1_render", 4096, (void *)0, 2, &eyes_task1, 1);
    xTaskCreatePinnedToCore(eyes_render_task, "eye2_render", 4096, (void *)1, 2, &eyes_task2, 1);

    vTaskSuspend(eyes_task1);
    vTaskSuspend(eyes_task2);

    return 0;
}

void eyes_render_task(void *arg)
{
    EyeController *control = eye_controller_get_instance();

    int eye_idx = (int)arg;

    const EventBits_t my_stopped_bit =
        (eye_idx == 0) ? EYE0_STOPPED_BIT : EYE1_STOPPED_BIT;

#if EYES_PERF_LOG
    /* 注意：不能用 last_frame_time = 0，eyes_get_tick_ms() 是"开机至今毫秒数"，
     * 否则第一次循环就会满足 >=1000 而打出假的 FPS:1。这里用 -1 表示尚未计时。 */
    int frame_count = 0;
    int last_frame_time = -1;
#endif

    while (1)
    {
        if (!s_eyes_running) {
            xEventGroupSetBits(s_eye_events, my_stopped_bit);
            vTaskSuspend(NULL);
            continue;
        }
        
        // int current_time = eyes_get_tick_ms();

        if(eye_idx == 1)
        {
            /* 游戏模式：检测超时 */
            if (logic_page_eyes_is_game_playing()) {
                logic_page_eyes_game_timeout_check();
            }

            /* 非游戏模式 → 正常情绪切换 & 行为更新 */
            if (!logic_page_eyes_is_game_playing()) {
                logic_emotion_auto_update();
                dooi_behavior_update();
            }
        }

        if (control->eye_disp_mode != control->eye_disp_next_mode && eye_idx == 1) {
            control->eye_disp_mode = control->eye_disp_next_mode;
            eyes_disp_mode_set(eye_idx, control->eye_disp_next_mode);
        }

        if(eye_controller_render_frame(eye_idx))
        {
            #if 0
            eye_controller_show_frame(eye_idx, &control.eye_disp_mode, &control.eye_disp_next_mode, control.eye_emotion_type);
            #else
            
            #endif
        }

        /* 残影修复：库里 render_frame() 只把脏区画进内存影子缓冲（hal_flush 不碰 SPI），
         * 由这里把「本帧变化的行」整段推上屏。
         * 必须放在 render_frame 之后：此时影子缓冲已经是完整的一帧。
         * 渲染被脏检测整帧跳过时，脏区为空，这个调用是零开销。 */
        eyes_port_disp_hal_publish(eye_idx);

#if EYES_PERF_LOG
        frame_count++;

        /* 每秒统计一次：帧数 / 被脏检测跳过的比例 / 平均每帧实际重绘行数 */
        {
            static uint32_t prev_rendered[2] = {0, 0};
            static uint32_t prev_rows[2]     = {0, 0};
            static uint32_t prev_pixels[2]   = {0, 0};

            int now = (int)eyes_get_tick_ms();
            if (last_frame_time < 0) {
                last_frame_time = now;   /* 第一秒只做基准，不打印 */
                frame_count = 0;
            } else if (now - last_frame_time >= 1000) {
                uint32_t rendered = 0, skipped = 0, rows = 0, pixels = 0;
                eye_controller_get_perf(eye_idx, &rendered, &skipped, &rows, &pixels);

                uint32_t d_render = rendered - prev_rendered[eye_idx];
                uint32_t d_rows   = rows - prev_rows[eye_idx];
                uint32_t d_px     = pixels - prev_pixels[eye_idx];
                prev_rendered[eye_idx] = rendered;
                prev_rows[eye_idx]     = rows;
                prev_pixels[eye_idx]   = pixels;

                /* kpx/s = 每秒实际推送的像素量，直接反映 SPI 负载 */
                printf("eye%d FPS:%u  skipped:%u%%  rows/f:%u  px/f:%u  MB/s:%.2f\n",
                       eye_idx, frame_count,
                       (unsigned)(frame_count ? ((frame_count - d_render) * 100 / frame_count) : 0),
                       (unsigned)(d_render ? (d_rows / d_render) : 0),
                       (unsigned)(d_render ? (d_px / d_render) : 0),
                       (double)d_px * 2.0 / 1048576.0);

                frame_count = 0;
                last_frame_time = now;
            }
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}