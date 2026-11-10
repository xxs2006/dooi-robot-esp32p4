/* =========================================================================
 * eyes_config.h —— Uncanny Eyes 编译期配置 + 数学工具
 *
 * 本文件只依赖 C 标准库，不含任何 OS / 硬件 / HAL 相关内容。
 * 所有平台相关的东西都在 eyes_port.h。
 *
 * 覆写方式（任选其一）：
 *   1) 编译选项： -DEYES_SCREEN_WIDTH=320 ...
 *   2) 在包含本头文件之前先 #define，本文件里所有宏都有 #ifndef 保护
 * ========================================================================= */
#ifndef EYES_CONFIG_H
#define EYES_CONFIG_H

#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "eyes_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------ 屏幕尺寸 ------------------------------ */
/* 默认按 240x240 圆屏（GC9A01）设计。方屏 / 其它分辨率请覆写这两个宏，
 * 同时记得把 EYES_ROUND_CLIP 设为 0（否则四角会被裁掉）。 */
#ifndef EYES_SCREEN_WIDTH
#define EYES_SCREEN_WIDTH  240
#endif

#ifndef EYES_SCREEN_HEIGHT
#define EYES_SCREEN_HEIGHT 240
#endif

/* 兼容旧代码里的 SCREEN_WIDTH / SCREEN_HEIGHT */
#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH  EYES_SCREEN_WIDTH
#endif
#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT EYES_SCREEN_HEIGHT
#endif

/* --------------------------- 分块渲染参数 ---------------------------- */
/* 一次 flush 的最大行数。受 DMA 缓冲大小限制，后端可通过
 * eyes_backend_t::max_chunk_lines 再调小（但不会超过这里的上限）。
 * 20 行时单块缓冲 28.8KB 会把 L2 冲掉一大块；10 行 4.8KB 查表能常驻。 */
#ifndef EYES_CHUNK_LINES
#define EYES_CHUNK_LINES   10
#endif
#define RENDER_CHUNK_HEIGHT EYES_CHUNK_LINES

/* ------------------------------ 圆屏裁剪 ------------------------------ */
/* 240x240 的 GC9A01 是圆屏，四角永远不可见。开启后每个分块只渲染/传输
 * 圆内的列区间，可省下约 15% 的像素计算和传输量。
 * 方屏必须置 0，否则四角会被裁掉。 */
#ifndef EYES_ROUND_CLIP
#define EYES_ROUND_CLIP    1
#endif

/* 可见圆域半径（像素）。240 圆屏为 120；若外壳有遮挡可略微调小（如 118）。 */
#ifndef EYES_CIRCLE_RADIUS
#define EYES_CIRCLE_RADIUS (EYES_SCREEN_WIDTH / 2)
#endif

/* --------------------------- 像素字节序 ------------------------------ */
/* 多数 SPI 小屏（ILI9xxx/GC9A01）要求 RGB565 大端传输，所以默认做字节交换。
 * 帧缓冲 / SDL / 上位机等"按主机序"的目标，运行时调用
 * eyes_set_pixel_swap(0) 即可，无需重新编译。 */
#ifndef EYES_PIXEL_SWAP16
#define EYES_PIXEL_SWAP16  1
#endif

#if defined(__GNUC__) || defined(__clang__)
#define EYES_SWAP16(x) ((uint16_t)__builtin_bswap16((uint16_t)(x)))
#else
#define EYES_SWAP16(x) ((uint16_t)((((uint16_t)(x)) >> 8) | (((uint16_t)(x)) << 8)))
#endif

#define FLOAT_EPSILON 0.0000001f  // 根据实际精度需求调整

/* 限制范围宏 */
#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))
#define ABS(x) ((x) < 0 ? -(x) : (x))

/* ------------------------------ 快速数学 ------------------------------ */
#ifndef USE_FAST_MATH
#define USE_FAST_MATH 1
#endif

#ifndef M_PI
#define M_PI 3.14f
#endif

#if USE_FAST_MATH
/* 近似 sin：基于 Schraudolph 近似修正（误差 ~1e-3 ~ 1e-2 量级） */
static inline float fast_sin(float x){
    /* wrap 到 [-pi, pi] */
    const float invTwoPi = (float)(1.0 / (2.0 * M_PI));
    x -= floorf(x * invTwoPi + 0.5f) * (float)(2.0 * M_PI);
    /* 粗近似 */
    float y = (float)(4.0 / M_PI) * x - (float)(4.0 / (M_PI * M_PI)) * x * fabsf(x);
    /* 修正项 */
    const float P = 0.225f;
    y = P * (y * fabsf(y) - y) + y;
    return y;
}
static inline float fast_cos(float x){ return fast_sin(x + (float)(M_PI * 0.5)); }

/* 快速 1/sqrt：Quake 近似 + 一次牛顿迭代 */
static inline float fast_inv_sqrt(float x){
    float xhalf = 0.5f * x;
    union { float f; uint32_t i; } u = { x };
    u.i = 0x5f3759df - (u.i >> 1);
    float y = u.f;
    y = y * (1.5f - xhalf * y * y);   /* 一次迭代足够 */
    return y;
}
static inline float fast_sqrt(float x){
    return x <= 0.0f ? 0.0f : 1.0f / fast_inv_sqrt(x);
}
#else
static inline float fast_sin(float x){ return sinf(x); }
static inline float fast_cos(float x){ return cosf(x); }
static inline float fast_sqrt(float x){ return sqrtf(x); }
#endif

static inline float fminf_s(float a, float b){ return a < b ? a : b; }
static inline float fmaxf_s(float a, float b){ return a > b ? a : b; }

/* [min, max) 区间内的随机整数 */
static inline uint32_t urandom(uint32_t min, uint32_t max){
    if (max <= min) return min;
    return (uint32_t)(eyes_rand32() % (max - min)) + min;
}

/* [min, max] 区间内均匀分布的随机浮点数。
 * 注意：eyes_rand32() 返回的是完整的 32 位随机数，不是 rand() 的 [0,RAND_MAX]，
 * 因此这里除以 0xFFFFFFFFu 才是均匀分布（旧代码除以 RAND_MAX 是个 bug，
 * 会让取值几乎总是撞到区间端点）。 */
static inline float frandom(float min, float max){
    if (max < min) { float t = min; min = max; max = t; }
    float r = (float)eyes_rand32() / (float)0xFFFFFFFFu;
    return min + r * (max - min);
}

#ifdef __cplusplus
}
#endif

#endif /* EYES_CONFIG_H */
