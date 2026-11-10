/* =========================================================================
 * eyes_fb.h —— 通用帧缓冲后端（跨平台、零依赖）
 *
 * 适用于：
 *   - PC / Linux / MCU 上位机：渲染到内存，再自己送屏或存文件
 *   - Linux framebuffer（/dev/fb0）：memcpy 到映射地址即可
 *   - 单元测试：直接读帧缓冲比对像素
 *
 * 用法：
 *   eyes_fb_t fb;
 *   eyes_fb_init(&fb, 2, 240, 240);     // 2 只眼，各 240x240
 *   eyes_fb_install(&fb);               // 注册为库后端（并关闭字节交换）
 *   eye_controller_init(&bigBlue, &bigBlue);
 *   while (1) {
 *       eye_controller_render_frame(0);
 *       eye_controller_render_frame(1);
 *       present(eyes_fb_get(&fb, 0), eyes_fb_get(&fb, 1));  // 你自己送屏
 *   }
 * ========================================================================= */
#ifndef EYES_FB_H
#define EYES_FB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t *buf;      /* eye_count 个 w*h 的帧缓冲，连续存放 */
    int       eye_count;
    int       w, h;
} eyes_fb_t;

/**
 * @brief 分配 eye_count 个 w*h 的 RGB565 帧缓冲。
 * @return 0 成功，负数失败
 */
int  eyes_fb_init(eyes_fb_t *fb, int eye_count, int w, int h);

/**
 * @brief 使用外部已存在的帧缓冲（不负责释放）。
 */
int  eyes_fb_attach(eyes_fb_t *fb, uint16_t *external_buf, int eye_count, int w, int h);

void eyes_fb_deinit(eyes_fb_t *fb);

/**
 * @brief 把该 fb 注册为库的后端，并自动关闭像素字节交换（主机序）。
 */
void eyes_fb_install(eyes_fb_t *fb);

/**
 * @brief 取某只眼的帧缓冲（只读渲染结果）。
 */
const uint16_t *eyes_fb_get(const eyes_fb_t *fb, int eye_index);

/**
 * @brief 用单色填充某只眼的帧缓冲。
 */
void eyes_fb_clear(eyes_fb_t *fb, int eye_index, uint16_t color);

#ifdef __cplusplus
}
#endif

#endif /* EYES_FB_H */
