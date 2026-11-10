/* ============== display_hal.h ============== */

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "driver/spi_master.h"
#include "lvgl.h"

/* ================================================================
 *  单屏改造：2.0 寸 ST7789v2 裸屏，320x240 横屏，插主板右座（FPC1）
 *
 *  2026-11-09 路线：
 *    上午 —— 左座 + 转接线 + 官方序列 + SPI Mode 3 一次点亮，红绿蓝全对，
 *            证明屏/线/座都没问题，之前全黑纯粹是驱动配置错；
 *    下午 —— 用户要求横屏显示，且把转接线改插右座 FPC1、左座空着。
 *            于是分辨率 320x240、引脚换回 LCD1_*、MADCTL 换 0x60（横屏正向）。
 *
 *  横屏的两处必然结果（不是 bug）：
 *    1) 时钟页 DIG_W 可以放大回 42（总宽 304 < 320），比竖屏时数字更大；
 *    2) 眼睛带每只 120x120、两只共 240 宽，在 320 宽里左右各留 40，垂直居中。
 *
 *  原来的双 1.28 寸 GC9A01 圆屏方案已停用；右大屏 ST7365 方案已停用。
 * ================================================================ */
#define LCD_H_RES           320
#define LCD_V_RES           240

/* ----------------------------------------------------------------
 *  屏幕调参区：下面 6 个宏是唯一需要按实物微调的地方，改完重编即可。
 *  测试画面就是用来一次性把这几项调准的：
 *    颜色不对（红蓝颠倒） -> DISP_ELE_ORDER_BGR
 *    黑白颠倒             -> DISP_INVERT_COLOR
 *    画面上下/左右反      -> DISP_MIRROR_X / DISP_MIRROR_Y
 *    颜色怪异、条纹噪点   -> DISP_TEST_SWAP_BYTES
 *    画面撕裂/雪花/闪屏   -> DISP_SPI_CLK_HZ 降到 20M
 * ---------------------------------------------------------------- */

/* 1 = 开机先跑纯色/彩条测试画面，不启动 LVGL 和眼睛渲染；0 = 正常 UI */
#define DISP_TEST_MODE       0

/* 裸屏走的是转接线，比原厂 FPC 长，40M 有丢包风险。
 * 先用 20M：240x320 全屏 150KB，20M 下一帧 60ms 左右，眼睛动画够用。
 * 如果出现撕裂/雪花/偶尔闪黑，降到 10M；确认稳定后再往上试。 */
#define DISP_SPI_CLK_HZ      (20 * 1000 * 1000)

/* SPI 时钟极性与相位。
 * ★ 必须是 3 ★ 官方 TFTLCD_GPIO_Init() 把 SCL 置高后就不再拉低，
 * 也就是空闲电平为高 = CPOL=1，配合上升沿采样 = CPHA=1。
 * 之前一直用 0，是这块屏「背光亮、全黑」的两大根因之一。 */
#define DISP_SPI_MODE        3

/* MADCTL（0x36）扫描方向。ST7789 只有标准四方向，绕一圈就这四个值：
 *   0x00 竖屏 240x320       0x60 横屏 320x240
 *   0xC0 竖屏 180°          0xA0 横屏 180°
 * 0x60（MV|MX）和 0xA0（MV|MY）是**互为 180° 的两个横屏方向**。
 *
 * 2026-11-09：先用 0xA0（官方 USE_HORIZONTAL=3 默认值），实测画面是倒的，
 * 用户要求 180°，换成 0x60 —— 这才是横屏正向。
 *
 * 再要微调只改这一个值：左右反 0x20，上下反 0xE0（都是非标准值，
 * 仅在 0x60/0xA0 都不对时才用）。 */
#define DISP_ST_MADCTL      0x60

/* 下面两个宏现在只影响 IDF 内置驱动那一瞬间的状态，
 * 真正的最终值由官方序列（0x36 MADCTL / 0x21 INVON）决定，见 display_hal.c。
 * 保留是为了兼容测试画面的代码路径。 */
#define DISP_ELE_ORDER_BGR   0                   /* 0 = RGB（实测正确） */
#define DISP_INVERT_COLOR    1                   /* 官方序列末尾发 0x21 INVON */
/* 硬件镜像（MADCTL 的 MX/MY 位）。
 * 小屏改版后**不再调用** esp_lcd_panel_mirror()：那会在官方序列之后
 * 重发 0x36，把 DISP_ST_MADCTL 覆盖掉，属于典型的「改了没反应」。
 * 方向一律通过上面一个 DISP_ST_MADCTL 控制，这两个宏保留 0 即可。 */
#define DISP_MIRROR_X        0
#define DISP_MIRROR_Y        0

/* 软件镜像：在写入 DMA 缓冲时逐行/逐列翻转，不碰 MADCTL，因此不会引起坐标偏移。
 * 只作用于眼睛直写通路（display_eyes_blit）。LVGL 页面暂不支持。
 *   水平翻转（左右对调） -> DISP_SW_MIRROR_X 置 1
 *   垂直翻转（上下对调） -> DISP_SW_MIRROR_Y 置 1 */
#define DISP_SW_MIRROR_X     0
#define DISP_SW_MIRROR_Y     0

/* 1 = 在眼睛页四角画 16x16 彩色标记，用于一次性确认坐标映射与方向：
 *     屏幕左上 红 / 右上 绿 / 左下 蓝 / 右下 黄
 *   四块都贴在玻璃边角 = 窗口和玻璃 1:1 对齐；若有偏移会差一大截。
 *   2026-11-03 已确认：红在左上、绿在右上、蓝在左下、黄在右下，坐标与方向全部正确，
 *   故关闭。以后若再动分辨率/镜像/眼睛尺寸，把它改回 1 复查一次。 */
#define DISP_PROBE_CORNERS   0
#define DISP_TEST_SWAP_BYTES 1                   /* 颜色变成紫/绿噪点时改成 0 */

/* 1 = 同时驱动两个 FPC 座：FPC1（右座，LCD1_* 引脚）和 FPC4（左座，LCD2_* 引脚），
 *     测试画面两边一起画。
 * 为什么需要：原理图上两个座的 LEDA/LEDK 是并联的，共用同一路背光升压，
 *   所以「屏插错座」时同样是背光亮、无画面，和驱动不匹配的现象无法区分。
 *   打开这个开关可以一次性排除「插错座」，屏在哪个座都能出图。
 * 确认屏插在哪个座之后改回 0，只驱动实际用到的那个座。
 * 仅在 DISP_TEST_MODE=1 时生效，不影响正常 UI。
 *
 * 2026-11-03：屏已确认插在右座 FPC1（LCD1_* 13/14/12/15），改回 0。
 * 原来置 1 时每一帧都要把同一张图往两个 SPI 总线各送一遍，白烧一半带宽，
 * 直接拖慢帧率、加剧动眼时的拖影感。 */
#define DISP_PROBE_BOTH      0

/* ----------------------------------------------------------------
 *  小屏验证模式（临时排障开关）
 *
 *  0 = 正常：右座 FPC1 / 大屏 ST7365 320x480（2026-11-05 确认满意的版本）
 *  1 = 验证：左座 FPC4 / 小屏 ST7789 240x320，只循环刷纯色后死循环
 *
 *  打开后 display_hal_init() 会先初始化左座小屏，然后进入纯色循环，不再
 *  启动 LVGL 和眼睛渲染。全程硬编码 240x320 且只用 LCD2_* 引脚，
 *  不修改 LCD_H_RES / LCD_V_RES，所以对正常大屏版本零影响。
 *
 *  用途是二选一定性：
 *    能看到 红→绿→蓝→白→黑 循环
 *        → 屏、转接线、左座引脚全部正常，之前不显示纯粹是驱动配置问题
 *          （分辨率 / 初始化序列 / 镜像反显），改回来就能用
 *    一直全黑（背光可能亮）
 *        → 屏、转接线或左座引脚有问题，先别动代码，去查线
 *
 *  2026-11-09 已通过：左座 + 转接线 + 官方 ST7789v2 序列 + SPI Mode 3，
 *  5 色循环正常、红绿蓝全部正确。结论是屏/线/座都是好的，问题在驱动配置。
 *  现在改回 0，正常 UI 直接跑在左座小屏上（见 display_hal_init 的引脚）。
 *  以后再怀疑屏坏/线断，把它改回 1 重烧即可复现这套纯色自检。
 * ---------------------------------------------------------------- */
#define DISP_SMALL_TEST      0

/* 小屏验证跑在哪个 FPC 座上（换座对照实验用）：
 *   0 = 左座 FPC4 (LCD2_*  GPIO29/30/28/31)  ← 小屏转接线现在插的座
 *   1 = 右座 FPC1 (LCD1_*  GPIO13/14/12/15)  ← 大屏在用、已被证明能驱动屏的座
 *
 * 用法：把「小屏 + 转接线」整套从左边拔下来插到右座，把这里改 1 重烧。
 *   右座出颜色 -> 屏和转接线都是好的，嫌疑落在左座 / 左座那几根线
 *   右座也全黑 -> 屏或转接线本身的问题，跟哪个座无关
 * 这样一次就把「软件配置错」和「接线错」彻底分开，不用万用表。
 * 验证完改回 0，再把 DISP_SMALL_TEST 改回 0 恢复大屏。 */
#define DISP_SMALL_TEST_ON_LCD1   0

/* 小屏验证用的 SPI 时钟。转接线比原厂 FPC 长，40M 容易丢数据。
 * 压到 2M：这一步只判断「能不能出画面」，速度完全不重要，
 * 信号完整性嫌疑先摘干净。确认能亮之后再逐步提到 20M / 40M。 */
#define DISP_SMALL_TEST_CLK_HZ  (2 * 1000 * 1000)

/* 小屏验证用的 SPI 模式（CPOL/CPHA）：
 *   3 = 空闲 SCL 高、上升沿采样 ← 官方 TFTLCD_GPIO_Init() 把 SCL SetBits，是这种
 *   0 = 空闲 SCL 低、上升沿采样 ← 之前一直用的，出不了画面
 * 两种都试过才知道哪种对。改这一个数字重烧即可切换。 */
#define DISP_SMALL_TEST_SPI_MODE   3

/* ----------------------------------------------------------------
 *  引脚连续性探测（配合万用表用）
 *
 *  1 = 启动后不初始化任何屏幕，改成让左座那 4 根信号线轮流拉高/拉低
 *      （每 2.5 秒翻转一次，串口会打印"现在测哪根"）。
 *      用户把万用表一端放 GND、另一端依次点到小屏的焊盘上，
 *      数字跟着 3.3V <-> 0V 跳 = 这根线真的通到屏上；
 *      数字纹丝不动 = 这根线断了、接错了、或压根没接到这个座。
 *
 *  为什么需要它：万用表看不出一根线"通到哪"，只能看出"通不通"。
 *  静态电平下（SPI 空闲）几乎所有脚都停在 3.3V，没法判断谁是谁；
 *  只有主动翻转才是唯一的客观证据。
 *
 *  只在排障时打开，正常固件必须为 0。
 *  2026-11-05 17:45 关闭：实测证明「通电状态用蜂鸣档/电压判断引脚」全是假象
 *  （DC 每帧只有约 60ms 是高，万用表读到的是平均值 0V；芯片输出低电平时
 *    对地蜂鸣档必响），已改用「换座对照 + 纯色画面」这条更硬的判据。
 * ---------------------------------------------------------------- */
#define DISP_PIN_PROBE       0

/* ----------------------------------------------------------------
 *  单屏双眼布局（正常 UI 生效）
 *
 *  little_eyes 是预编译库，画布尺寸编译期固定 240x240，改不了。
 *  所以后端把每只眼 2:1 降采样成 120x120，再左右并排贴到屏幕中间：
 *
 *      [ 左眼 120 ][ 右眼 120 ]        x = (320-240)/2 = 40 起
 *                                      y = (240-120)/2 = 60 起
 *
 *  横屏 320 宽下，眼睛带两侧各留 40 像素黑边；240 高下上下各留 60。
 *  这是 little_eyes 只能用整数降采样的硬限制：scale=1 就是两只 240 宽、
 *  共 480 宽，装不下。想让眼睛更大只能改成「裁剪」而不是「缩放」，
 *  属于下一步的事，这里先保持满宽居中。
 * ---------------------------------------------------------------- */
#define DISP_EYE_SCALE       2      /* 240 / 2 = 120 */
#define DISP_EYE_W           (240 / DISP_EYE_SCALE)
#define DISP_EYE_H           (240 / DISP_EYE_SCALE)
#define DISP_EYE_GAP         0
#define DISP_EYE_X0          ((LCD_H_RES - (DISP_EYE_W * 2 + DISP_EYE_GAP)) / 2)
#define DISP_EYE_Y0          ((LCD_V_RES - DISP_EYE_H) / 2)

/* 1 = 眼睛画面同时写两个 FPC 座（插哪个座都出图，代价是 SPI 带宽减半）；
 * 0 = 只写 FPC1 右座。确认屏插在哪个座后改成 0 可以让动画更顺。 */
#define DISP_EYES_BLIT_ALL   1

/* 分块渲染高度（每块 240 x CHUNK_LINES 像素）。
 * 10 行 = 4.8KB 缓冲，能让查表常驻 L2；调大省事务开销但会增加内存。 */
#define CHUNK_LINES         10                                /* 每块高度，320/10=32块 */
#define CHUNK_PIXELS        (LCD_H_RES * CHUNK_LINES)         /* 2400 像素 */
#define CHUNK_BUF_BYTES     (CHUNK_PIXELS * sizeof(uint16_t)) /* 4800 字节 */

/* 眼睛渲染的 ping-pong 缓冲数量。
 * 必须是 3：注册了 on_color_trans_done 回调后 draw_bitmap 不会阻塞等传输完成，
 * 只有 2 个缓冲时，第 3 次提交就会写进"还在被 DMA 读"的那块缓冲 → 撕裂。
 * 3 个缓冲 + 严格轮转的安全性证明见 display_hal.c 里的注释。
 * 注意：trans_queue_depth 必须 <= DISP_BUF_COUNT，否则这个论证不成立。 */
#define DISP_BUF_COUNT      3

/* 只驱动一个物理屏：右座 FPC1（LCD1_PIN_*，GPIO13/14/12/15）。
 * 逻辑上的 DISPLAY_LEFT / DISPLAY_RIGHT 都映射到这块屏（见 display_get_dev）。 */
#define NUM_DISPLAYS        1

#define LCD1_HOST          SPI2_HOST   // HSPI
#define LCD2_HOST          SPI3_HOST   // VSPI

// /*==================== LCD1 引脚定义 ====================*/
#define LCD1_PIN_SCLK      GPIO_NUM_13
#define LCD1_PIN_MOSI      GPIO_NUM_14
#define LCD1_PIN_CS        GPIO_NUM_NC // 不需要片选
#define LCD1_PIN_DC        GPIO_NUM_12
#define LCD1_PIN_RST       GPIO_NUM_15
#define LCD1_PIN_BL        GPIO_NUM_16 // 共用背光

// /*==================== LCD2 引脚定义 ====================*/
#define LCD2_PIN_SCLK      GPIO_NUM_29
#define LCD2_PIN_MOSI      GPIO_NUM_30
#define LCD2_PIN_CS        GPIO_NUM_NC // 不需要片选
#define LCD2_PIN_DC        GPIO_NUM_28
#define LCD2_PIN_RST       GPIO_NUM_31
#define LCD2_PIN_BL        GPIO_NUM_16  

/* 单个物理显示设备 */
typedef struct {
    /* ---- 硬件层 ---- */
    spi_host_device_t         host;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t    panel;

    /* ---- 渲染缓冲 (启动时预分配, DMA-safe)，眼睛渲染与 LVGL 共用 ----
     * 共用是安全的：两者互斥运行。眼睛页面激活时，眼睛任务直接写 panel、
     * LVGL 在该屏上没有内容可画；切到 LVGL 页面时 eyes_screen_unload() 会把
     * 两个眼睛任务 vTaskSuspend 掉。因此同一时刻只有一方在用这批缓冲。
     * LVGL 用 [0] 和 [1] 做双缓冲，眼睛渲染用全部 3 块做轮转。 */
    uint16_t *chunk_buf[DISP_BUF_COUNT];
    uint8_t   write_idx;          /* CPU 当前写入的 buffer 索引 */

    volatile uint32_t trans_done_count;   /* DMA 完成计数，仅用于观测 */
    SemaphoreHandle_t trans_done_sem;     /* 单块传输完成信号量（测试画面逐块刷新要等它） */

    /* ---- LVGL 关联 ---- */
    lv_display_t *lv_disp;       /* LVGL 显示对象 */

    /* ---- 引脚 ---- */
    int pin_sclk, pin_mosi, pin_cs, pin_dc, pin_rst;
} display_dev_t;

/* API */
void display_hal_init(void);
void display_send_chunk_rect(int dev_idx, int x_start, int y_start, int x_end, int y_end, uint16_t *data);
void display_send_chunk(int dev_idx, int y_start, int y_end, uint16_t *data);
void display_send_chunk_y(int dev_idx, int y_start, int y_end, uint16_t *data);
uint16_t *display_get_write_buf(int dev_idx);
void display_swap_buf(int dev_idx);
display_dev_t *display_get_dev(int dev_idx);
void display_set_panel_mirror_x(int dev_idx, bool mirror_x);

/* 把一小块 RGB565 图像贴到屏幕指定位置（x,y,w,h，像素在 src 里行主序紧密排列）。
 * 内部会拷进该屏的轮转 DMA 缓冲再提交，所以 src 可以是栈/静态缓冲。
 * 单屏双眼渲染用它把降采样后的眼睛块送到面板。 */
void display_eyes_blit(int x, int y, int w, int h, const uint16_t *src);

/* 四角彩色标记（红左上/绿右上/蓝左下/黄右下），用于确认坐标映射与方向 */
void display_draw_probe_corners(void);

/* 把所有在用面板整屏刷成同一个颜色（RGB565，已按面板字节序处理）。
 * 进入眼睛页时用它把残留的 LVGL 画面清掉，同时给眼睛带做出黑色背景。 */
void display_clear_all(uint16_t color);

void lvgl_displays_init(void);

/* 纯色 + 彩条 + 边框角标测试画面，内部死循环，用于验证 ST7789 驱动是否打通。
 * 仅在 DISP_TEST_MODE=1 时调用，不依赖 LVGL。 */
void display_test_pattern_run(void);
