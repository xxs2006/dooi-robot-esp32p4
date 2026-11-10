/* =========================================================================
 * eyes_port.h —— Uncanny Eyes 跨平台移植接口
 *
 * 库本身不认识任何硬件：它只会
 *   1) 向后端要一块写入缓冲（可选，DMA 零拷贝）
 *   2) 往里画一个矩形区域的 RGB565 像素
 *   3) 把这块矩形交给后端 flush
 * 再加上时钟 / 随机数 / 临界区 / 内存分配 这几个可覆盖的钩子。
 *
 * 最小接入：只实现 flush 即可（见 eyes_set_flush_backend）。
 * ========================================================================= */
#ifndef EYES_PORT_H
#define EYES_PORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 把一个矩形区域的像素推给显示设备。
 *
 * @param user        eyes_backend_t::user
 * @param eye_index   眼睛/屏幕索引（0 起）
 * @param x0,y0,x1,y1 矩形范围，右/下开区间
 * @param pixels      行主序、紧密打包的 (x1-x0) * (y1-y0) 个 RGB565 像素。
 *                    字节序由 eyes_set_pixel_swap() 决定（默认大端，适配 SPI 屏）。
 *                    该指针只在本次回调内有效。
 *
 * 注意：库保证矩形不跨行缓冲边界，且同一时刻只有一个线程调用 flush
 *      （两只眼各自一个渲染线程时，请提供 lock/unlock）。
 */
typedef void (*eyes_flush_cb)(void *user, int eye_index,
                              int x0, int y0, int x1, int y1,
                              const uint16_t *pixels);

typedef struct eyes_backend {
    /* ---- 必需 ---- */
    eyes_flush_cb flush;

    /* ---- 可选 ---- */
    /* 申请一块可写缓冲，用于零拷贝（例如直接写 DMA 缓冲）。
     * 返回 NULL 时库会使用内部静态缓冲（每只眼一份）。 */
    uint16_t *(*acquire)(void *user, int eye_index, size_t pixels);

    /* 显示模式切换（镜像）。不需要就留 NULL。 */
    void      (*set_mirror)(void *user, int eye_index, bool mirror_x);

    /* 毫秒时钟；NULL 用平台默认实现 */
    uint32_t  (*tick_ms)(void *user);

    /* 32 位随机数；NULL 用平台默认实现（xorshift32） */
    uint32_t  (*rand32)(void *user);

    /* 临界区：多线程渲染 / 外部改状态时保护共享状态；NULL 用平台默认实现 */
    void      (*lock)(void *user);
    void      (*unlock)(void *user);

    /* 内存分配：需要 PSRAM / 指定 heap 时覆盖；NULL 用 malloc/free */
    void     *(*alloc)(void *user, size_t size);
    void      (*free)(void *user, void *ptr);

    /* 单次 flush 的最大行数（受 DMA 缓冲限制）；0 = 用 EYES_CHUNK_LINES */
    int        max_chunk_lines;

    void      *user;
} eyes_backend_t;

/**
 * @brief 注册后端。内部会拷贝一份，传入的可以是栈上的临时结构体。
 */
void eyes_set_backend(const eyes_backend_t *backend);

/**
 * @brief 取出当前后端（内部拷贝的指针），未设置时返回 NULL。
 */
const eyes_backend_t *eyes_get_backend(void);

/**
 * @brief 最简注册：只提供 flush，其余全部走默认实现。
 */
void eyes_set_flush_backend(eyes_flush_cb flush, void *user);

/**
 * @brief 设置输出像素是否做字节交换（默认 EYES_PIXEL_SWAP16）。
 *        SPI 小屏一般要 1；帧缓冲 / SDL / 上位机一般要 0。
 */
void eyes_set_pixel_swap(int enable);
int  eyes_pixel_swap(void);

/* ---------------------- 供库内部使用的平台封装 ---------------------- */
/* 外部代码一般不需要直接调用，但移植层可以复用。 */
uint32_t eyes_get_tick_ms(void);
uint32_t eyes_rand32(void);
void     eyes_enter_critical(void);
void     eyes_exit_critical(void);
void    *eyes_alloc(size_t size);
void     eyes_free(void *ptr);
int      eyes_get_chunk_lines(void);

#ifdef __cplusplus
}
#endif

#endif /* EYES_PORT_H */
