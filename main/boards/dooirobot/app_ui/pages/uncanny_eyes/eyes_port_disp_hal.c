/* =========================================================================
 * eyes_port_disp_hal.c —— Uncanny Eyes 库 <-> 本工程 display_hal 的桥接
 *
 * 库侧接口见 uncanny_eyes/include/eyes_port.h。
 * 换屏 / 换硬件时，只需要改这个文件。
 *
 * ---------------------------------------------------------------------
 * 一、屏幕布局（单屏改造）
 * ---------------------------------------------------------------------
 * little_eyes 是**预编译库**，画布尺寸编译期固定 240x240（没有源码，改不了）。
 * 而硬件只剩一块 320x480 的屏，要把两只眼放进同一块屏，这里做两件事：
 *   1) 每只眼按 2:1 降采样：240x240 -> 120x120
 *   2) 按 eye_index 加横向偏移：左眼贴 (40,180)，右眼贴 (160,180)
 *
 * ---------------------------------------------------------------------
 * 二、为什么必须走「影子帧缓冲 + 整段推送」（残影修复的核心）
 * ---------------------------------------------------------------------
 * 库为了省 SPI 带宽，不是每帧整屏刷新，而是逐行做脏检测，把**变化的行**
 * 合成 1~3 行的 DirtyRun（见 eyes_core.h）单独 flush 出来。
 *
 * 早期实现是「来一个 run 就立刻送一屏」，结果有两个致命问题：
 *   a) 降采样时用 y%2 抽行，奇数行开头的 run 整块被丢弃 -> 那一行永远不更新；
 *   b) 即使一行不漏，屏幕在任意瞬间也是「多个时刻的拼接」——瞳孔在这几行已经是
 *      新位置，在那几行还是上一帧的旧位置，动眼时看起来就是挥之不去的残影/重影。
 *
 * 现在的做法：
 *   - hal_flush() 只把像素写进内存里的影子帧缓冲，**不碰 SPI**；
 *   - 每帧渲染结束后由 eyes_port_disp_hal_publish() 把「上次推送以来所有变化的行」
 *     合并成一块**连续的行区间**，一次性整段推上屏。
 *   于是屏幕上每一块区域要么是完整的旧帧、要么是完整的新帧，不会出现半新半旧的
 *   拼接；而且整段推送 = 连续 SPI 事务，天然比零散小 run 快得多。
 *
 * px/f 反而更省：静止时脏区间为空，一个字节都不推。
 * ========================================================================= */
#include "eyes_port_disp_hal.h"

#include <string.h>

#include "eyes_port.h"
#include "eyes_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "display_hal.h"

static const char *TAG = "eyes_port";

/* 一次 SPI 事务最多推多少行。
 * 受 display_hal 的轮转缓冲大小限制（CHUNK_BUF_BYTES = 320x10x2 = 6400B），
 * 120 宽的话 6400/(120*2) = 26 行，取 24 留余量。 */
#define EYE_STRIP_ROWS   ((CHUNK_BUF_BYTES / (DISP_EYE_W * (int)sizeof(uint16_t))) & ~1)

/* 每只眼的影子帧缓冲（降采样后的 120x120） */
static uint16_t        *s_shadow[2]   = { NULL, NULL };
static SemaphoreHandle_t s_blit_mutex = NULL;
static bool             s_need_swap   = true;

/* 待推送的行区间 [dy0, dy1)；dy0 >= dy1 表示没有任何变化。
 * 每次 hal_flush 往里并区间，publish 时整段推完再清空。 */
static int s_dy0[2] = { DISP_EYE_H, DISP_EYE_H };
static int s_dy1[2] = { 0, 0 };

/* ------------------------------ 像素工具 ------------------------------ */
static inline uint16_t sw16(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

/* 两个相邻像素做 RGB565 平均。
 * 输入缓冲里的字节序由 eyes_set_pixel_swap() 决定（SPI 屏是大端，即已交换），
 * 直接对交换过的 16bit 值做算术会把 R/G/B 的位搅在一起，所以先还原再平均。 */
static inline uint16_t eyes_avg2(uint16_t a, uint16_t b)
{
    if (s_need_swap) { a = sw16(a); b = sw16(b); }

    unsigned r = (((a >> 11) & 0x1Fu) + ((b >> 11) & 0x1Fu)) >> 1;
    unsigned g = (((a >>  5) & 0x3Fu) + ((b >>  5) & 0x3Fu)) >> 1;
    unsigned c = (( a        & 0x1Fu) + ( b        & 0x1Fu)) >> 1;

    uint16_t out = (uint16_t)((r << 11) | (g << 5) | c);
    return s_need_swap ? sw16(out) : out;
}

/* ------------------------------- 输出 ------------------------------- */
static uint16_t *hal_acquire(void *user, int eye_index, size_t pixels)
{
    (void)user;
    (void)eye_index;
    (void)pixels;
    /* 返回 NULL：让库用它自己"每只眼一份"的内部缓冲。
     * 不能把 display_hal 的轮转缓冲给它——两只眼的渲染任务会抢同一块，
     * 而且我们本来就要先降采样一遍才能进影子缓冲。 */
    return NULL;
}

/* 库把一块脏矩形交给我们：只更新影子缓冲，不上屏 */
static void hal_flush(void *user, int eye_index,
                      int x0, int y0, int x1, int y1,
                      const uint16_t *pixels)
{
    (void)user;

    if (pixels == NULL)                       return;
    if (x1 <= x0 || y1 <= y0)                 return;
    if (eye_index < 0 || eye_index > 1)       return;

    uint16_t *shadow = s_shadow[eye_index];
    if (shadow == NULL)                       return;

    const int src_w = x1 - x0;

    /* 源矩形 [x0,x1) x [y0,y1) 要「完整」映射到输出网格上：
     * 左/上边界向下取整、右/下边界向上取整，把不足一组的边缘行、列也并进来。
     * 只要写得进影子缓冲就不会丢内容，这里仍然这么做是为了保证 2:1 缩放严格
     * 覆盖每一个源像素（否则边缘会出现 1 像素的采样空洞，放大后能看见）。 */
    const int ox0 = x0 / DISP_EYE_SCALE;
    const int ox1 = (x1 - 1) / DISP_EYE_SCALE;
    const int oy0 = y0 / DISP_EYE_SCALE;
    const int oy1 = (y1 - 1) / DISP_EYE_SCALE;

    for (int oy = oy0; oy <= oy1; oy++) {
        if (oy < 0 || oy >= DISP_EYE_H) continue;

        /* 该输出行对应的源行：优先取偶数行（保持固定的采样相位），
         * 若不在脏区里就夹到脏区最近的一行。 */
        int sy = oy * DISP_EYE_SCALE;
        if (sy < y0)     sy = y0;
        if (sy > y1 - 1) sy = y1 - 1;

        const uint16_t *sp = pixels + (size_t)(sy - y0) * (size_t)src_w;
        uint16_t       *dp = shadow + (size_t)oy * (size_t)DISP_EYE_W;

        for (int ox = ox0; ox <= ox1; ox++) {
            if (ox < 0 || ox >= DISP_EYE_W) continue;

            /* 该输出列对应的两个源列；脏区只覆盖半组时重复用组内那一个 */
            int c0 = ox * DISP_EYE_SCALE - x0;
            int c1 = c0 + 1;
            if (c0 < 0)       c0 = 0;
            if (c0 > src_w-1) c0 = src_w - 1;
            if (c1 < 0)       c1 = 0;
            if (c1 > src_w-1) c1 = src_w - 1;

            dp[ox] = eyes_avg2(sp[c0], sp[c1]);   /* 水平方向 2:1 平均 */
        }

        /* 合并脏行区间 */
        if (oy < s_dy0[eye_index])     s_dy0[eye_index] = oy;
        if (oy + 1 > s_dy1[eye_index]) s_dy1[eye_index] = oy + 1;
    }
}

/* ---------------------------------------------------------------------
 * 把某只眼「自上次推送以来变化的那些行」整段推上屏。
 * 由渲染任务在一帧渲染完成后调用（见 eyes_control.c）。
 * ------------------------------------------------------------------- */
void eyes_port_disp_hal_publish(int eye_index)
{
    if (eye_index < 0 || eye_index > 1) return;

    const uint16_t *shadow = s_shadow[eye_index];
    if (shadow == NULL) return;

    int y0 = s_dy0[eye_index];
    int y1 = s_dy1[eye_index];

    /* 先清脏区再推：推的过程中若库又 flush 了新内容，下一帧会继续处理，
     * 不会因为这里清空而丢掉更新。 */
    s_dy0[eye_index] = DISP_EYE_H;
    s_dy1[eye_index] = 0;

    if (y0 >= y1) return;
    if (y0 < 0)               y0 = 0;
    if (y1 > DISP_EYE_H)      y1 = DISP_EYE_H;

    const int dx = DISP_EYE_X0 + eye_index * (DISP_EYE_W + DISP_EYE_GAP);
    const int dy = DISP_EYE_Y0 + y0;
    const int total_rows = y1 - y0;

    /* 两只眼是两个独立任务、同一块物理屏，必须串行化 */
    if (s_blit_mutex) xSemaphoreTake(s_blit_mutex, portMAX_DELAY);

    for (int r = 0; r < total_rows; r += EYE_STRIP_ROWS) {
        int rows = total_rows - r;
        if (rows > EYE_STRIP_ROWS) rows = EYE_STRIP_ROWS;

        display_eyes_blit(dx, dy + r, DISP_EYE_W, rows,
                          shadow + (size_t)(y0 + r) * (size_t)DISP_EYE_W);
    }

    if (s_blit_mutex) xSemaphoreGive(s_blit_mutex);
}

static void hal_set_mirror(void *user, int eye_index, bool mirror_x)
{
    (void)user;
    (void)eye_index;
    (void)mirror_x;
    /* 单屏双眼：两块逻辑屏合成一块物理屏，镜像整屏会把左右眼对调。
     * 所以这里不做任何事；屏幕方向统一交给 display_hal.h 的 DISP_MIRROR_X/Y。 */
}

/* --------------------------- 时钟 / 随机数 --------------------------- */
static uint32_t hal_tick_ms(void *user)
{
    (void)user;
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint32_t hal_rand32(void *user)
{
    (void)user;
    return esp_random();
}

/* ------------------------------ 临界区 ------------------------------ */
/* 物理引擎（idx==1 的任务）会写 control.eyes[]，两只眼的渲染任务都会读它，
 * 必须真实加锁，否则会读到"半更新"的状态组合。临界区只包住标量拷贝，很短。 */
static portMUX_TYPE s_eye_mux = portMUX_INITIALIZER_UNLOCKED;

static void hal_lock(void *user)
{
    (void)user;
    portENTER_CRITICAL(&s_eye_mux);
}

static void hal_unlock(void *user)
{
    (void)user;
    portEXIT_CRITICAL(&s_eye_mux);
}

static void *hal_alloc(void *user, size_t sz)
{
    (void)user;
    return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void hal_free(void *user, void *ptr)
{
    (void)user;
    heap_caps_free(ptr);
}

/* ------------------------------ 安装 ------------------------------ */
static uint16_t *shadow_alloc(void)
{
    size_t bytes = (size_t)DISP_EYE_W * DISP_EYE_H * sizeof(uint16_t);

    /* 优先片内 RAM：影子缓冲每帧被写穿、又被按行读出来推给 SPI，
     * 放 PSRAM 会多一次缓存 miss。片内不够才退到 PSRAM。 */
    uint16_t *p = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (p) {
        memset(p, 0, bytes);
    }
    return p;
}

void eyes_port_disp_hal_install(void)
{
    for (int i = 0; i < 2; i++) {
        if (s_shadow[i] == NULL) {
            s_shadow[i] = shadow_alloc();
            if (s_shadow[i] == NULL) {
                ESP_LOGE(TAG, "eye%d shadow alloc failed (%d bytes)",
                         i, (int)(DISP_EYE_W * DISP_EYE_H * sizeof(uint16_t)));
            }
        }
        /* 首次必须整屏推一次 */
        s_dy0[i] = 0;
        s_dy1[i] = DISP_EYE_H;
    }

    if (s_blit_mutex == NULL) {
        s_blit_mutex = xSemaphoreCreateMutex();
    }

    eyes_backend_t be;
    memset(&be, 0, sizeof(be));

    be.flush           = hal_flush;
    be.acquire         = hal_acquire;
    be.set_mirror      = hal_set_mirror;
    be.tick_ms         = hal_tick_ms;
    be.rand32          = hal_rand32;
    be.lock            = hal_lock;
    be.unlock          = hal_unlock;
    be.alloc           = hal_alloc;
    be.free            = hal_free;
    be.max_chunk_lines = CHUNK_LINES;      /* 与 HAL 的分块缓冲高度保持一致 */
    be.user            = NULL;

    eyes_set_backend(&be);

    /* SPI 小屏要求 RGB565 大端 */
    eyes_set_pixel_swap(1);
    s_need_swap = (eyes_pixel_swap() != 0);

    ESP_LOGI(TAG, "影子帧缓冲就绪 %dx%d x2, 单次推送最多 %d 行",
             DISP_EYE_W, DISP_EYE_H, EYE_STRIP_ROWS);
}
