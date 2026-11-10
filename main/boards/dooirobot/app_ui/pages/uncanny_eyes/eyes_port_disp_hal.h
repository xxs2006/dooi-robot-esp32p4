/* =========================================================================
 * eyes_port_disp_hal.h —— 把 Uncanny Eyes 库接到本工程的 display_hal
 *
 * 库是跨平台的（不知道任何硬件），这一层告诉它：
 *   - 往哪块缓冲写（display_get_write_buf，DMA 零拷贝）
 *   - 怎么把矩形推到屏上（display_send_chunk / _rect）
 *   - 怎么切镜像（display_set_panel_mirror_x）
 *   - 时钟 / 随机数 / 临界区 / 内存用什么
 * ========================================================================= */
#ifndef EYES_PORT_DISP_HAL_H
#define EYES_PORT_DISP_HAL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 安装后端。必须在 eye_controller_init() / eyes_render_init() 之前调用。
 */
void eyes_port_disp_hal_install(void);

/**
 * @brief 把某只眼「自上次推送以来变化的行」整段推上屏。
 *
 * 库的 flush 只写内存影子缓冲，不上屏；渲染任务每完成一帧就调一次这个函数，
 * 由它把脏行区间合并成连续块一次性推送 —— 屏幕不会出现「半新半旧」的拼接，
 * 这是动眼残影/重影的根治手段。没有任何变化时调用开销为 0。
 *
 * @param eye_index 0 或 1
 */
void eyes_port_disp_hal_publish(int eye_index);

#ifdef __cplusplus
}
#endif

#endif /* EYES_PORT_DISP_HAL_H */
