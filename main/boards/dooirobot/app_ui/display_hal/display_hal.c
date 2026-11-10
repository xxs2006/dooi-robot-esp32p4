/* ============== display_hal.c ============== */
#include "display_hal.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_st7789.h"   /* 单屏改造：ST7789 驱动由 IDF 内置 esp_lcd 提供 */
#include <string.h>
#include "driver/gpio.h"
#include "freertos/task.h"

static const char *TAG = "disp_hal";

display_dev_t display_dev[NUM_DISPLAYS] = {0}; /* 左屏[0], 右屏[1] */

/* 测试模式专用：第二个探测屏（另一个 FPC 座）。
 * 独立于 display_dev[]，这样 NUM_DISPLAYS 仍为 1，不会影响 LVGL 正常模式的单屏逻辑。 */
#if DISP_PROBE_BOTH
static display_dev_t s_probe_dev;
static bool          s_probe_ok = false;
#endif

/* 测试画面当前要画的目标列表 */
static display_dev_t *s_test_targets[2];
static int            s_test_target_n = 0;

/* ------------------------------------------------------------------ */
/*  DMA 完成回调（ISR 上下文）                                          */
/* ------------------------------------------------------------------ */
/*
 * 原来这里直接调 lv_display_flush_ready()：一来 LVGL 的 API 不是 ISR-safe 的，
 * 二来 LVGL 现在用自己的缓冲了，眼睛渲染的完成和 LVGL 没有任何关系。
 * 这里只保留一个计数用于观测（需要更严格流控时可以接成信号量）。
 *
 * 缓冲区的安全性不依赖这个回调，而是依赖 DISP_BUF_COUNT=3 的严格轮转，
 * 证明见 display_advance_buf() 的注释。
 */
static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                           esp_lcd_panel_io_event_data_t *edata,
                                           void *user_ctx)
{
    (void)io;
    (void)edata;

    display_dev_t *dev = (display_dev_t *)user_ctx;
    if (dev) {
        dev->trans_done_count++;

        /* 测试画面逐块刷新时靠这个信号量做流控，避免 CPU 改写还在被 DMA 读的缓冲 */
        if (dev->trans_done_sem) {
            BaseType_t woken = pdFALSE;
            xSemaphoreGiveFromISR(dev->trans_done_sem, &woken);
            return (woken == pdTRUE);
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  display_hal 公共 API                                                */
/* ------------------------------------------------------------------ */
/*
 * 获取当前可写入的 buffer 指针
 */
uint16_t *display_get_write_buf(int dev_idx)
{
    display_dev_t *dev = &display_dev[dev_idx];
    return dev->chunk_buf[dev->write_idx];
}

/*
 * 轮转推进写指针。
 *
 * 为什么 3 个缓冲 + 严格轮转是安全的（无论 draw_bitmap 内部是阻塞还是异步）：
 *   设 SPI 事务队列深度为 D、缓冲数为 N=3，第 k 次提交用的是 buffer (k mod 3)。
 *   提交第 k 次时，若在飞事务数已达 D，spi_device_queue_trans 会阻塞，
 *   直到最早提交的那次（第 k-D 次）完成才返回。SPI 严格 FIFO，所以第 k-D 次完成时，
 *   更早的第 k-D-1、k-D-2 ... 次必然早已完成。
 *   接下来要写的是第 k 次提交用的 buffer = (k mod 3)。
 *   只要 D <= 3，就有 k-D >= k-3，而 (k-3) mod 3 == k mod 3 —— 也就是说
 *   "当前要写的这块 buffer，3 次提交之前用过，那次必定已经完成了"。
 * 所以 N=3 且 trans_queue_depth <= 3 时安全。
 * 原来 N=2、D=2 是不安全的：第 3 次提交会写进第 1 次提交还在用的那块 buffer。
 */
static inline void display_advance_buf(display_dev_t *dev)
{
    dev->write_idx = (uint8_t)((dev->write_idx + 1) % DISP_BUF_COUNT);
}

/*
 * 发送局部的脏矩形数据到 LCD，并推进 buffer
 * 仅传输发生变化的视口区域 [x_start, y_start] 到 [x_end, y_end]
 */
void display_send_chunk_rect(int dev_idx, int x_start, int y_start, int x_end, int y_end, uint16_t *data)
{
    display_dev_t *dev = &display_dev[dev_idx];

    // 注意：esp_lcd_panel_draw_bitmap 的参数 x_end 和 y_end 是开区间 (exclusive)
    esp_lcd_panel_draw_bitmap(dev->panel, x_start, y_start, x_end, y_end, data);

    display_advance_buf(dev);
}

/*
 * 单屏双眼渲染专用：把一小块 RGB565 图像贴到面板指定位置。
 * 和 display_send_chunk_rect 的区别是「先拷贝进目标屏自己的轮转缓冲」——
 * 这样同一份源数据（眼睛降采样后的块）可以安全地依次送给两块屏，
 * 每块屏的 DMA 各读各的缓冲，不会互相踩。
 */
void display_eyes_blit(int x, int y, int w, int h, const uint16_t *src)
{
    if (src == NULL || w <= 0 || h <= 0) return;
    if ((size_t)w * (size_t)h * sizeof(uint16_t) > CHUNK_BUF_BYTES) return;

    /* 软件镜像：镜像整块矩形（含位置），等价于整屏翻转，不改 MADCTL */
    int dx = x;
    int dy = y;
#if DISP_SW_MIRROR_X
    dx = LCD_H_RES - (x + w);
#endif
#if DISP_SW_MIRROR_Y
    dy = LCD_V_RES - (y + h);
#endif
    if (dx < 0 || dy < 0 || dx + w > LCD_H_RES || dy + h > LCD_V_RES) return;

    int limit = (s_test_target_n > 0) ? s_test_target_n : 1;
#if !DISP_EYES_BLIT_ALL
    limit = 1;
#endif
    for (int t = 0; t < limit; t++) {
        display_dev_t *dev = (s_test_target_n > 0) ? s_test_targets[t] : &display_dev[0];
        uint16_t *dst = dev->chunk_buf[dev->write_idx];
#if DISP_SW_MIRROR_X || DISP_SW_MIRROR_Y
        for (int r = 0; r < h; r++) {
            int sr = DISP_SW_MIRROR_Y ? (h - 1 - r) : r;
            const uint16_t *s = src + (size_t)sr * (size_t)w;
            uint16_t       *d = dst + (size_t)r  * (size_t)w;
            for (int c = 0; c < w; c++) {
                d[c] = s[DISP_SW_MIRROR_X ? (w - 1 - c) : c];
            }
        }
#else
        memcpy(dst, src, (size_t)w * (size_t)h * sizeof(uint16_t));
#endif
        esp_lcd_panel_draw_bitmap(dev->panel, dx, dy, dx + w, dy + h, dst);
        display_advance_buf(dev);
    }
}

/* 四角彩色标记：红(左上) 绿(右上) 蓝(左下) 黄(右下)。
 * 用来一次性确认「窗口坐标 (0,0) 是否真的落在玻璃左上角」以及方向是否需要镜像。 */
void display_draw_probe_corners(void)
{
    const int S = 16;
    static const uint16_t rgb[4] = { 0xF800, 0x07E0, 0x001F, 0xFFE0 };

    const int px[4] = { 0, LCD_H_RES - S, 0,             LCD_H_RES - S };
    const int py[4] = { 0, 0,             LCD_V_RES - S, LCD_V_RES - S };

    int limit = (s_test_target_n > 0) ? s_test_target_n : 1;

    for (int t = 0; t < limit; t++) {
        display_dev_t *dev = (s_test_target_n > 0) ? s_test_targets[t] : &display_dev[0];
        for (int c = 0; c < 4; c++) {
            uint16_t *dst = dev->chunk_buf[dev->write_idx];
            uint16_t be = (uint16_t)((rgb[c] >> 8) | (rgb[c] << 8));  /* RGB565 大端 */
            for (int i = 0; i < S * S; i++) dst[i] = be;
            esp_lcd_panel_draw_bitmap(dev->panel, px[c], py[c], px[c] + S, py[c] + S, dst);
            display_advance_buf(dev);
        }
    }
}

/*
 * 整屏刷成同一颜色（按块发送，缓冲复用轮转的 chunk_buf）。
 * 进眼睛页之前用它清掉上一页残留的 LVGL 画面，顺便给眼睛带铺黑底。
 */
void display_clear_all(uint16_t color)
{
    int limit = (s_test_target_n > 0) ? s_test_target_n : 1;

    for (int t = 0; t < limit; t++) {
        display_dev_t *dev = (s_test_target_n > 0) ? s_test_targets[t] : &display_dev[0];

        for (int y = 0; y < LCD_V_RES; y += CHUNK_LINES) {
            int ye = y + CHUNK_LINES;
            if (ye > LCD_V_RES) ye = LCD_V_RES;

            uint16_t *buf = dev->chunk_buf[dev->write_idx];
            int n = LCD_H_RES * (ye - y);
            for (int i = 0; i < n; i++) {
                buf[i] = color;
            }
            esp_lcd_panel_draw_bitmap(dev->panel, 0, y, LCD_H_RES, ye, buf);
            display_advance_buf(dev);
        }
    }
}

void display_send_chunk(int dev_idx, int y_start, int y_end, uint16_t *data)
{
    display_dev_t *dev = &display_dev[dev_idx];

    esp_lcd_panel_draw_bitmap(dev->panel, 0, y_start, LCD_H_RES, y_end, data);

    display_advance_buf(dev);
}

/*
 * 仅发送发生行变化的垂直区间 [y_start, y_end)，水平方向保持全宽 240 像素。
 * 这样既能实现跳过计算/传输的性能提升，又保证了 DMA 内存跨步的绝对安全，不会花屏。
 */
void display_send_chunk_y(int dev_idx, int y_start, int y_end, uint16_t *data)
{
    display_dev_t *dev = &display_dev[dev_idx];
    
    // 渲染区域从 0 到 LCD_H_RES (240)，高度为局部的 y_start 到 y_end
    esp_lcd_panel_draw_bitmap(dev->panel, 0, y_start, LCD_H_RES, y_end, data);

    display_advance_buf(dev);
}

void display_set_panel_mirror_x(int dev_idx, bool mirror_x)
{
    display_dev_t *dev = &display_dev[dev_idx];

    esp_lcd_panel_mirror(dev->panel, mirror_x, false);

    char data[2] = {0x00, 0x00};
    esp_lcd_panel_draw_bitmap(dev->panel, 0, 0, 1, 1, data);

    display_advance_buf(dev);
}

/* ------------------------------------------------------------------ */
/*  厂 家 面 板 专 用 初 始 化 序 列                                      */
/* ------------------------------------------------------------------ */
/*
 * 来源：原厂 Keil 例程 TFT-2.00-T200H7-C12-04-ST7789-4SPI-320X240 的
 *       User/main.c -> TFT_init()，逐条照搬，未做任何改动。
 *
 * 为什么必须补这一段：
 *   ESP-IDF 内置的 esp_lcd_panel_st7789 只在 panel_init() 里发 4 条命令
 *   （SWRESET / SLPOUT / MADCTL / COLMOD），完全没有电源、VCOM、VRH、
 *   porch、帧率、gamma 这些模拟参数配置。这块 IPS 裸屏缺了这些配置的
 *   典型表现就是：背光亮着，但屏幕纯黑，看不到任何图案。
 *
 * 三条最关键的：
 *   0xBB VCOM 电压 = 0x21  —— 对比度命脉，缺了就是"亮背光 + 黑屏"
 *   0xC3 VRH  电压 = 0x0B  —— 灰阶参考电压
 *   0x21 INVON             —— IPS 面板必须开反显
 */
/* ⚠ 已废弃：这是 T200H7 那批面板的序列，和现在这块 ST7789v2 参数不符
 * （见 st7789v2_official_init 里的对照表）。正常 UI 不再调用它，
 * 只留在这里做对照，万一换回旧批次面板可以快速切回来。 */
static void __attribute__((unused)) st7789_vendor_init(esp_lcd_panel_io_handle_t io)
{
    /* 0x36: 显示扫描方向 —— 0xC0 = MY|MX，整屏 180°，与原厂 USE_HORIZONTAL=1 一致 */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x36, (uint8_t[]){0xC0}, 1));
    /* 0x3A: 像素格式 16bit RGB565 */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x3A, (uint8_t[]){0x05}, 1));
    /* 0xB2: porch 前后肩设置 */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xB2, (uint8_t[]){0x05, 0x05, 0x00, 0x33, 0x33}, 5));
    /* 0xB7: 栅极电压 VGH/VGL */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xB7, (uint8_t[]){0x35}, 1));
    /* 0xBB: VCOM 电压 ★ */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xBB, (uint8_t[]){0x21}, 1));
    /* 0xC0: LCM control */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC0, (uint8_t[]){0x2C}, 1));
    /* 0xC2: VDV/VRH 命令使能 */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC2, (uint8_t[]){0x01}, 1));
    /* 0xC3: VRH 电压 ★ */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC3, (uint8_t[]){0x0B}, 1));
    /* 0xC4: VDV 电压 */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC4, (uint8_t[]){0x20}, 1));
    /* 0xC6: 帧率 111Hz（点反转） */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC6, (uint8_t[]){0x01}, 1));
    /* 0xD0: 电源控制 —— 原厂连发两次：先 0xA7 打开，再 0xA4 设定，照搬 */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xD0, (uint8_t[]){0xA7, 0xA1}, 2));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xD0, (uint8_t[]){0xA4, 0xA1}, 2));
    /* 0xD6: 厂商保留寄存器，原厂固定 0xA1 */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xD6, (uint8_t[]){0xA1}, 1));
    /* 0xE0: 正极性 gamma，14 级 */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xE0,
        (uint8_t[]){0xD0, 0x04, 0x08, 0x0A, 0x09, 0x05, 0x2D, 0x43,
                    0x49, 0x09, 0x16, 0x15, 0x26, 0x2B}, 14));
    /* 0xE1: 负极性 gamma，14 级 */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xE1,
        (uint8_t[]){0xD0, 0x03, 0x09, 0x0A, 0x0A, 0x06, 0x2E, 0x44,
                    0x40, 0x3A, 0x15, 0x15, 0x26, 0x2A}, 14));
    /* 0x21: 开启反显（IPS 面板必须），对应原厂 TFT_SEND_CMD(0x21) */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x21, NULL, 0));
}

/* ================================================================== */
/*  引脚连续性探测（DISP_PIN_PROBE=1）                                  */
/* ================================================================== */
/*
 * 目的：把「信号线上到底有没有信号」这件事变成万用表能看见的东西。
 *
 * SPI 空闲时 SCLK/MOSI/DC/RST 大多停在 3.3V，静态测量分不清谁是谁、
 * 也分不清线通不通。这里干脆跳出 SPI，把这 4 根脚当普通 GPIO 用，
 * 每根轮流「拉高 2.5 秒 -> 拉低 2.5 秒」，串口同步广播当前正在测哪根。
 *
 * 判读方法（表笔一端固定 GND）：
 *   数字在 3.3V 和 0V 之间跟着串口提示跳 -> 这根线通到了该焊盘
 *   数字一直是 0 或一直是 3.3V 不动        -> 断了 / 接错脚 / 没接到这个座
 *
 * 注意：探测期间不初始化任何屏幕，所以屏幕只会亮背光，这是预期的。
 */
#if DISP_PIN_PROBE
typedef struct {
    int         gpio;
    const char *name;
    const char *where;   /* 小屏 12P 排线上的脚位（按镜像接法的编号） */
} probe_pin_t;

static const probe_pin_t s_probe_pins[] = {
    /* 脚号以「小屏 2.0" ST7789 12P 裸屏」丝印为准：
     *   1 GND / 2 LEDK / 3 LEDA / 4 VDD / 5 GND / 6 GND /
     *   7 DCX / 8 CSX / 9 SCL / 10 SDA / 11 RESX / 12 GND
     * ★ 注意 7 脚是 DCX、8 脚是 CSX —— 不是 TE/SDO，绝不能接地。 */
    { LCD2_PIN_SCLK, "SCLK", "小屏第 9 脚 SCL" },
    { LCD2_PIN_MOSI, "MOSI", "小屏第 10 脚 SDA" },
    { LCD2_PIN_DC,   "DC",   "小屏第 7 脚 DCX ★关键" },
    { LCD2_PIN_RST,  "RST",  "小屏第 11 脚 RESX" },
};

static void pin_probe_task(void *arg)
{
    const int n = (int)(sizeof(s_probe_pins) / sizeof(s_probe_pins[0]));

    for (int i = 0; i < n; i++) {
        gpio_config_t cfg = {
            .mode         = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << s_probe_pins[i].gpio,
        };
        gpio_config(&cfg);
        gpio_set_level(s_probe_pins[i].gpio, 0);
    }

    ESP_LOGW(TAG, "##################################################");
    ESP_LOGW(TAG, "  引脚探测模式：4 根信号线轮流翻转，每根 2.5 秒");
    ESP_LOGW(TAG, "  表笔一端接 GND，另一端点到小屏对应焊盘上");
    ESP_LOGW(TAG, "  屏幕只亮背光、不出画面，这是正常的");
    ESP_LOGW(TAG, "##################################################");

    vTaskDelay(pdMS_TO_TICKS(3000));

    for (;;) {
        for (int i = 0; i < n; i++) {
            const probe_pin_t *p = &s_probe_pins[i];
            ESP_LOGW(TAG, "");
            ESP_LOGW(TAG, ">>> 现在测 %s  (GPIO%d)  表笔点：%s  <<<",
                     p->name, p->gpio, p->where);
            for (int c = 0; c < 2; c++) {
                gpio_set_level(p->gpio, 1);
                ESP_LOGW(TAG, "      %s = 高电平  -> 万用表应显示约 3.3V", p->name);
                vTaskDelay(pdMS_TO_TICKS(2500));
                gpio_set_level(p->gpio, 0);
                ESP_LOGW(TAG, "      %s = 低电平  -> 万用表应显示 0V", p->name);
                vTaskDelay(pdMS_TO_TICKS(2500));
            }
        }
        ESP_LOGW(TAG, "");
        ESP_LOGW(TAG, ">>> 四根测完，从头再来一轮 <<<");
    }
}
#endif /* DISP_PIN_PROBE */

/* ================================================================== */
/*  官方 ST7789V2 初始化序列（正常 UI 与小屏自检共用）                    */
/* ================================================================== */
/*
 * 来源：D:\Desktop\dooi\40-tft_2_00_1\APP\tftlcd\tftlcd.c 的 TFT==200 分支
 * 该包 readme 写明：2.0 寸 240x320，IC = ST7789v2。
 *
 * 和之前移植的那套（T200H7 包）参数完全不是一回事，而这些恰好都是
 * 「背光亮、全黑」的典型成因：
 *
 *   命令               官方 ST7789v2      旧 T200H7
 *   0xB2 porch         0C 0C 00 33 33    05 05 00 33 33
 *   0x20 反显关闭       有                 无
 *   0xB7 栅极电压       0x56               0x35
 *   0xBB VCOMS         0x18               0x21
 *   0xC3 VRH           0x1F               0x0B
 *   0xC6 帧率           0x0F               0x01
 *   0xD0 电源          0xA6,0xA1          0xA7,0xA1 再 0xA4,0xA1
 *   0xE0/E1 gamma      另一组             另一组
 *   0xE7 双通道 SPI    0x00（关闭）        无
 *
 * 这次逐条照抄，不做任何「差不多就行」的合并。
 *
 * 调用时机：必须在 esp_lcd_panel_init() **之后**。内置驱动只会发
 * SWRESET/SLPOUT/MADCTL/COLMOD 四条，这里把 MADCTL、COLMOD、反显、
 * 电源、VCOM、gamma 全部重新写一遍，最终状态完全由本函数决定。
 */
static void st7789v2_official_init(esp_lcd_panel_io_handle_t io, uint8_t madctl)
{
    const uint8_t b2[] = { 0x0C, 0x0C, 0x00, 0x33, 0x33 };
    const uint8_t d0[] = { 0xA6, 0xA1 };
    const uint8_t e0[] = { 0xD0, 0x0D, 0x14, 0x0B, 0x0B, 0x07, 0x3A, 0x44,
                           0x50, 0x08, 0x13, 0x13, 0x2D, 0x32 };
    const uint8_t e1[] = { 0xD0, 0x0D, 0x14, 0x0B, 0x0B, 0x07, 0x3A, 0x44,
                           0x50, 0x08, 0x13, 0x13, 0x2D, 0x32 };
    const uint8_t v_b7 = 0x56, v_bb = 0x18, v_c0 = 0x2C, v_c2 = 0x01;
    const uint8_t v_c3 = 0x1F, v_c4 = 0x20, v_c6 = 0x0F;
    const uint8_t v_colmod = 0x05, v_e7 = 0x00;

    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x36, &madctl, 1));   /* 扫描方向 */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xB2, b2, sizeof(b2))); /* porch */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x20, NULL, 0));      /* 反显关闭 */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xB7, &v_b7, 1));     /* 栅极电压 */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xBB, &v_bb, 1));     /* VCOMS ★ */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC0, &v_c0, 1));     /* LCM control */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC2, &v_c2, 1));     /* VDV/VRH 使能 */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC3, &v_c3, 1));     /* VRH ★ */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC4, &v_c4, 1));     /* VDV */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC6, &v_c6, 1));     /* 帧率 */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xD0, d0, sizeof(d0))); /* 电源控制 */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xE0, e0, sizeof(e0))); /* 正 gamma */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xE1, e1, sizeof(e1))); /* 负 gamma */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x3A, &v_colmod, 1)); /* RGB565 */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xE7, &v_e7, 1));     /* 关双通道 SPI */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x21, NULL, 0));      /* 反显开 */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x11, NULL, 0));      /* 退出睡眠 */
    vTaskDelay(pdMS_TO_TICKS(120));                                     /* 手册要求 */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x29, NULL, 0));      /* 显示开 */
}

/* 硬件复位：官方 TFTLCD_Init() 的做法是 RST 低 100ms、高 100ms。
 * 参数化是因为小屏自检可能切到左座/右座，两种引脚不同。 */
static void __attribute__((unused)) st_hw_reset_gpio(int rst_gpio)
{
    gpio_config_t rc = {
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << rst_gpio,
    };
    gpio_config(&rc);
    gpio_set_level(rst_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

/* ================================================================== */
/*  小屏验证模式（DISP_SMALL_TEST=1）                                   */
/* ================================================================== */
#if DISP_SMALL_TEST
/*
 * 刻意不复用 init_one_display()：
 *   它按 LCD_H_RES / LCD_V_RES（320x480，大屏参数）分配缓冲、发窗口坐标，
 *   套在小屏（240x320）上会越界。这里全部硬编码，和正常版本互不干扰。
 *
 * 只做一件事：全屏刷纯色并循环。不启 LVGL、不画眼睛、不做任何缩放。
 * 目的就是把「驱动配置不对」和「屏/接线有问题」区分开。
 */
/* ---- 座选择：一个宏切换整组引脚，换座对照实验就靠它 ---- */
#if DISP_SMALL_TEST_ON_LCD1
#  define ST_HOST        LCD1_HOST
#  define ST_PIN_SCLK    LCD1_PIN_SCLK
#  define ST_PIN_MOSI    LCD1_PIN_MOSI
#  define ST_PIN_CS      LCD1_PIN_CS
#  define ST_PIN_DC      LCD1_PIN_DC
#  define ST_PIN_RST     LCD1_PIN_RST
#  define ST_SOCK_NAME   "FPC1 右座（大屏那个座，已验证能驱动屏）"
#else
#  define ST_HOST        LCD2_HOST
#  define ST_PIN_SCLK    LCD2_PIN_SCLK
#  define ST_PIN_MOSI    LCD2_PIN_MOSI
#  define ST_PIN_CS      LCD2_PIN_CS
#  define ST_PIN_DC      LCD2_PIN_DC
#  define ST_PIN_RST     LCD2_PIN_RST
#  define ST_SOCK_NAME   "FPC4 左座（小屏转接线插的这个座）"
#endif

/* 小屏自检沿用上面参数化版本的硬复位 */
#define st_hw_reset()  st_hw_reset_gpio(ST_PIN_RST)

/* 两种可能的原生几何。2.0 寸 ST7789 竖屏是 240x320，横屏是 320x240；
 * 窗口和玻璃方向对不上的那一套，写入会被面板整片丢弃（看起来就是全黑）。
 * 所以两套轮流试：哪一套出颜色，就说明这块玻璃的原生扫描方向是哪个。
 * MADCTL 取自官方 tftlcd.h 的 USE_HORIZONTAL 分支：
 *   0 -> 0x00（竖）  3 -> 0xA0（横，MV 置位） */
typedef struct {
    int         w;
    int         h;
    uint8_t     madctl;
    const char *label;
} st_geom_t;

/* 只保留官方默认配置。之前两套几何轮换是个错误：官方 tftlcd.h 里
 * USE_HORIZONTAL 的默认值就是 3，也就是 320x240 + MADCTL 0xA0。
 * 轮换会让「什么时候该看到画面」变得不可知，反而干扰判断。 */
static const st_geom_t s_st_geoms[] = {
    { 320, 240, 0xA0, "320x240横/0xA0 官方默认" },
};

#define ST_LINES        20                                    /* 每块 20 行 */
#define ST_BUF_BYTES    (320 * ST_LINES * sizeof(uint16_t))   /* 12800 字节 */
#define ST_COLOR_MS     4000                                  /* 每种颜色停留 4 秒 */
#define ST_MARK         40                                    /* 左上角标记方块边长 */

/* 把 [y0, y0+rows) 这些行填成 color，并在左上角画一个反色方块。
 * 这个方块同时回答两个问题：有没有画面、坐标原点在哪个角。
 *
 * 这里主动把高低字节调换：面板吃大端，而 uint16_t 在 ESP32 里是小端存储，
 * 直接填 0xF800 到内存会变成 0x00,0xF8。不依赖任何驱动层的 swap 开关。 */
static void st_fill_block(uint16_t *buf, int w, int y0, int rows,
                          uint16_t color, uint16_t mark)
{
    uint16_t c = (uint16_t)((color << 8) | (color >> 8));
    uint16_t m = (uint16_t)((mark  << 8) | (mark  >> 8));

    for (int r = 0; r < rows; r++) {
        int       y    = y0 + r;
        uint16_t *line = buf + (size_t)r * (size_t)w;
        for (int x = 0; x < w; x++) {
            line[x] = (y < ST_MARK && x < ST_MARK) ? m : c;
        }
    }
}

/* 官方写法：2A/2B 的结束坐标是「包含」的，不是常见的开区间 */
static void st_set_window(esp_lcd_panel_io_handle_t io,
                          int x0, int y0, int x1, int y1)
{
    uint8_t caset[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0,
                         (uint8_t)(x1 >> 8), (uint8_t)x1 };
    uint8_t raset[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0,
                         (uint8_t)(y1 >> 8), (uint8_t)y1 };
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x2A, caset, sizeof(caset)));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x2B, raset, sizeof(raset)));
}

static void st_draw_geom(esp_lcd_panel_io_handle_t io, uint16_t *buf,
                         const st_geom_t *g)
{
    static const uint16_t colors[] = { 0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000 };
    static const char    *names[]  = { "RED", "GREEN", "BLUE", "WHITE", "BLACK" };

    for (int c = 0; c < (int)(sizeof(colors) / sizeof(colors[0])); c++) {
        uint16_t mark = (colors[c] == 0xFFFF) ? 0x0000 : 0xFFFF;
        ESP_LOGW(TAG, "[%s] fill %-5s (0x%04X)", g->label, names[c], colors[c]);

        for (int y = 0; y < g->h; y += ST_LINES) {
            int rows = g->h - y;
            if (rows > ST_LINES) rows = ST_LINES;

            st_fill_block(buf, g->w, y, rows, colors[c], mark);
            st_set_window(io, 0, y, g->w - 1, y + rows - 1);
            ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(
                io, 0x2C, buf, (size_t)g->w * (size_t)rows * sizeof(uint16_t)));
        }
        /* 必须让出 CPU：否则 IDLE 任务饿死会触发任务看门狗 */
        vTaskDelay(pdMS_TO_TICKS(ST_COLOR_MS));
    }
}

/* 跑一遍完整的「复位 → 软复位 → 初始化 → 刷 5 色」。
 *
 * 注意：这里**不销毁 io**。之前试过 esp_lcd_panel_io_del() 之后重建，
 * 结果第一轮刷完就 panic（Load access fault），设备进入重启循环。
 * 现在 io 只建一次，模式由宏固定，靠反复「复位+重新初始化」来循环。 */
static void st_run_cycle(esp_lcd_panel_io_handle_t io, uint16_t *buf)
{
    st_hw_reset();

    /* 软复位兜底：0x01 SWRESET。RESX 那根线若不通，这条也进不去；
     * 但若 RESX 只是悬空（内部上拉、上电状态不确定），它能把片子拉回确定状态。 */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x01, NULL, 0));
    vTaskDelay(pdMS_TO_TICKS(150));

    st7789v2_official_init(io, s_st_geoms[0].madctl);
    st_draw_geom(io, buf, &s_st_geoms[0]);
}

static void small_panel_test_run(void)
{
    ESP_LOGW(TAG, "===== SMALL PANEL TEST (官方 ST7789v2 序列) =====");
    ESP_LOGW(TAG, "座  : %s", ST_SOCK_NAME);
    ESP_LOGW(TAG, "引脚: SCLK=%d MOSI=%d DC=%d RST=%d  (SPI %dMHz)",
             ST_PIN_SCLK, ST_PIN_MOSI, ST_PIN_DC, ST_PIN_RST,
             DISP_SMALL_TEST_CLK_HZ / 1000000);
    ESP_LOGW(TAG, "不用 IDF 内置 ST7789 驱动，全部命令自己发（逐条照抄官方 tftlcd.c）");
    ESP_LOGW(TAG, "固定官方默认几何 320x240 / MADCTL 0xA0（USE_HORIZONTAL=3）");
    ESP_LOGW(TAG, "SPI Mode = %d（改 DISP_SMALL_TEST_SPI_MODE 宏切换）",
             DISP_SMALL_TEST_SPI_MODE);
    ESP_LOGW(TAG, "5 色各 %d 秒；左上角 40x40 反色小方块 = 坐标原点", ST_COLOR_MS / 1000);

    spi_bus_config_t bus_cfg = {
        .sclk_io_num     = ST_PIN_SCLK,
        .mosi_io_num     = ST_PIN_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = ST_BUF_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(ST_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = ST_PIN_CS,
        .dc_gpio_num       = ST_PIN_DC,
        .spi_mode          = DISP_SMALL_TEST_SPI_MODE,
        .pclk_hz           = DISP_SMALL_TEST_CLK_HZ,
        .trans_queue_depth = 2,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)ST_HOST, &io_cfg, &io));

    uint16_t *buf = (uint16_t *)heap_caps_malloc(
        ST_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    assert(buf && "small test buf alloc failed");

    for (;;) {
        st_run_cycle(io, buf);
    }
}
#endif /* DISP_SMALL_TEST */

/* ------------------------------------------------------------------ */
/*  初始化                                                              */
/* ------------------------------------------------------------------ */

static void init_one_display(display_dev_t *dev)
{
    ESP_LOGI(TAG, "Init ST7789 display on SPI host %d", dev->host);

    /* 0. 传输完成信号量（ISR 里 give，测试画面里 take） */
    dev->trans_done_sem = xSemaphoreCreateBinary();
    assert(dev->trans_done_sem && "trans_done_sem create failed!");

    /* 1. SPI 总线 */
    spi_bus_config_t bus_cfg = {
        .sclk_io_num     = dev->pin_sclk,
        .mosi_io_num     = dev->pin_mosi,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = CHUNK_BUF_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(dev->host, &bus_cfg, SPI_DMA_CH_AUTO));

    /* 2. Panel IO —— 注册 DMA 完成回调 */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num          = dev->pin_cs,
        .dc_gpio_num          = dev->pin_dc,
        .spi_mode             = DISP_SPI_MODE,
        .pclk_hz              = DISP_SPI_CLK_HZ,
        /* ★ 必须是 2，不能是 3 ★
         * 填缓冲发生在提交「之前」：写第 k 次用的 buffer(k mod 3) 时，
         * 队列里可能还排着 k-1、k-2、k-3 三笔在飞事务（D=3 时），
         * 而 (k+3) mod 3 == k mod 3 —— 也就是正在写的那块，恰好是 k-3 还在被 DMA 读的那块。
         * 结果就是画面出现彩色雪花噪点 + 撕裂。
         * 取 D=2 后，在飞事务只有 k-1、k-2，(k mod 3) 与它们互不相等，轮转才真正安全。 */
        .trans_queue_depth    = 2,
        .on_color_trans_done  = on_color_trans_done, /* ★ 关键回调 ★ */
        .user_ctx             = dev,
        .lcd_cmd_bits         = 8,
        .lcd_param_bits       = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)dev->host, &io_cfg, &dev->io));

    /* 3. ST7789 Panel（240x320，16bpp，数据按大端发送，与 LVGL 侧的 rgb565_swap 一致） */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = dev->pin_rst,
        .rgb_ele_order  = DISP_ELE_ORDER_BGR ? LCD_RGB_ELEMENT_ORDER_BGR
                                             : LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian    = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(dev->io, &panel_cfg, &dev->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(dev->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(dev->panel));

    /* ★★ 关键一步 ★★
     * 内置 st7789 驱动只发了 SWRESET/SLPOUT/MADCTL/COLMOD 四条命令，
     * 这里补上原厂完整序列（电源/VCOM/VRH/porch/帧率/gamma/反显）。
     * 少了这一段，这块 IPS 屏就是「背光亮着但全黑」。 */
    st7789v2_official_init(dev->io, DISP_ST_MADCTL);

    /* ★ 这三行千万别加回来 ★
     * esp_lcd_panel_invert_color() 会重发 0x20/0x21，
     * esp_lcd_panel_mirror()       会重发 0x36（把 DISP_ST_MADCTL 冲掉），
     * esp_lcd_panel_disp_on_off()  会重发 0x29。
     * 官方序列已经把这三件事都做完了，再调一遍就是自相矛盾。 */

    ESP_LOGI(TAG, "ST7789 %dx%d ready (clk=%dMHz, bgr=%d, inv=%d, mx=%d, my=%d)",
             LCD_H_RES, LCD_V_RES, DISP_SPI_CLK_HZ / 1000000,
             DISP_ELE_ORDER_BGR, DISP_INVERT_COLOR, DISP_MIRROR_X, DISP_MIRROR_Y);

    /* 4. 预分配渲染 Buffer（3 块，眼睛渲染与 LVGL 共用）
     * 优先放片内 SRAM：这些缓冲每帧被反复写穿，落在 PSRAM 上会把 L2 冲得更狠。 */
    for (int i = 0; i < DISP_BUF_COUNT; i++) {
        dev->chunk_buf[i] = (uint16_t *)heap_caps_malloc(
            CHUNK_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);

        if (!dev->chunk_buf[i]) {
            dev->chunk_buf[i] = (uint16_t *)heap_caps_malloc(
                CHUNK_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
        }
        assert(dev->chunk_buf[i] && "Chunk buffer alloc failed!");
        memset(dev->chunk_buf[i], 0, CHUNK_BUF_BYTES);
    }

    dev->write_idx = 0;
    dev->trans_done_count = 0;

    ESP_LOGI(TAG, "Display on host %d ready. Buf: %p / %p",
             dev->host, dev->chunk_buf[0], dev->chunk_buf[1]);
}

void display_hal_init(void)
{
#if DISP_PIN_PROBE
    /* 排障：不碰任何屏幕，只把 4 根信号线做成 3.3V/0V 慢速翻转，
     * 让万用表能一根根验证"主控 -> 屏焊盘"的连通性。 */
    xTaskCreate(pin_probe_task, "pin_probe", 3072, NULL, 3, NULL);
    return;
#endif

#if DISP_SMALL_TEST
    /* 小屏验证：只驱动左座 FPC4，刷完纯色后死循环，不进入正常 UI。
     * 验证完把 display_hal.h 里的 DISP_SMALL_TEST 改回 0 重烧即可恢复大屏。 */
    small_panel_test_run();
    return;
#endif

    /* 只驱动一块屏：右座 FPC1（小屏转接线现在插这里，左座空着）。
     * 两个座的引脚完全不同：右座 13/14/12/15，左座 29/30/28/31。
     * 引脚跟着「线插哪个座」走，和屏本身无关 —— 屏还是那块 ST7789v2，
     * SPI 模式、初始化序列都不用因为换座而改。 */
    display_dev[0] = (display_dev_t){
        .host      = LCD1_HOST,
        .pin_sclk  = LCD1_PIN_SCLK, .pin_mosi = LCD1_PIN_MOSI,
        .pin_cs    = LCD1_PIN_CS,   .pin_dc   = LCD1_PIN_DC,
        .pin_rst   = LCD1_PIN_RST,
    };

    /* 背光 */
    // gpio_config_t bl_cfg = {
    //     .mode         = GPIO_MODE_OUTPUT,
    //     .pin_bit_mask = 1ULL << LCD2_PIN_BL,
    // };
    // gpio_config(&bl_cfg);
    // gpio_set_level(LCD2_PIN_BL, 1);

    init_one_display(&display_dev[0]);

#if DISP_PROBE_BOTH
    /* 探测第二个座（FPC4 / 左座，LCD2_* 引脚）。
     * 两个座背光并联，插错座也只会「亮背光、无画面」，无法用眼睛区分，
     * 所以测试模式下干脆两路都点起来。 */
    s_probe_dev = (display_dev_t){
        .host      = LCD2_HOST,
        .pin_sclk  = LCD2_PIN_SCLK, .pin_mosi = LCD2_PIN_MOSI,
        .pin_cs    = LCD2_PIN_CS,   .pin_dc   = LCD2_PIN_DC,
        .pin_rst   = LCD2_PIN_RST,
    };
    init_one_display(&s_probe_dev);
    s_probe_ok = true;
    ESP_LOGW(TAG, "DISP_PROBE_BOTH=1：FPC1（右座 13/14/12/15）与 FPC4（左座 29/30/28/31）同时驱动");
#endif

    /* 测试画面目标：先右座，再（若开启）左座 */
    s_test_target_n = 0;
    s_test_targets[s_test_target_n++] = &display_dev[0];
#if DISP_PROBE_BOTH
    if (s_probe_ok) {
        s_test_targets[s_test_target_n++] = &s_probe_dev;
    }
#endif
}

/* 单屏改造：上层页面仍然按 DISPLAY_LEFT / DISPLAY_RIGHT 取设备，
 * 这里把越界的索引统一夹到 0，避免 assert 崩溃。 */
display_dev_t *display_get_dev(int dev_idx)
{
    if (dev_idx < 0 || dev_idx >= NUM_DISPLAYS) {
        dev_idx = 0;
    }
    return &display_dev[dev_idx];
}

/* ================================================================== */
/*  LVGL 层                                                             */
/* ================================================================== */

typedef struct {
    int dev_idx;
    lv_display_t *disp;
} lvgl_flush_ctx_t;

static lvgl_flush_ctx_t s_flush_ctx[NUM_DISPLAYS];

static void lvgl_flush_cb(lv_display_t *disp,
                          const lv_area_t *area,
                          uint8_t *color_p)
{
    lvgl_flush_ctx_t *ctx =
        (lvgl_flush_ctx_t *)lv_display_get_user_data(disp);

    display_dev_t *dev =
        &display_dev[ctx->dev_idx];

    uint32_t px_count =
        (uint32_t)lv_area_get_width(area) *
        (uint32_t)lv_area_get_height(area);

    lv_draw_sw_rgb565_swap(color_p, px_count);

    esp_lcd_panel_draw_bitmap(
        dev->panel,
        area->x1,
        area->y1,
        area->x2 + 1,
        area->y2 + 1,
        color_p
    );

    /* LVGL 自己的 buffer，draw_bitmap 返回后就可以立刻通知 LVGL 了。
     * 原来的做法是让 on_color_trans_done 回调去调 lv_display_flush_ready()，
     * 但那个回调跑在中断上下文，而 LVGL 的 API 不是 ISR-safe 的。 */
    lv_display_flush_ready(disp);
}
/* ------------------------------------------------------------------ */

static esp_timer_handle_t s_lv_tick_timer;

static void lv_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(10);
}

void lvgl_displays_init(void)
{
    lv_init();

    const esp_timer_create_args_t tcfg = {
        .callback = lv_tick_cb,
        .name     = "lv_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&tcfg, &s_lv_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_lv_tick_timer, 10000));

    for (int i = 0; i < NUM_DISPLAYS; i++) {
        display_dev_t *dev = &display_dev[i];

        /* 创建 LVGL display */
        lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);

        s_flush_ctx[i].dev_idx = i;
        s_flush_ctx[i].disp    = disp;
        lv_display_set_user_data(disp, &s_flush_ctx[i]);
        lv_display_set_flush_cb(disp, lvgl_flush_cb);

        /* 与眼睛渲染共用缓冲：见 display_hal.h 里 chunk_buf 的注释。
         * LVGL 拿 [0]/[1] 做双缓冲，眼睛渲染拿全部 3 块做轮转，两者互斥运行。 */
        lv_display_set_buffers(disp,
                               dev->chunk_buf[0],
                               dev->chunk_buf[1],
                               CHUNK_BUF_BYTES,
                               LV_DISPLAY_RENDER_MODE_PARTIAL);

        dev->lv_disp = disp;

        lv_disp_set_default(disp);
        lv_theme_t *theme = lv_theme_default_init(
            lv_disp_get_default(),
            lv_palette_main(LV_PALETTE_BLUE),
            lv_palette_main(LV_PALETTE_RED),
            true,
            LV_FONT_DEFAULT);
        lv_disp_set_theme(lv_disp_get_default(), theme);

        ESP_LOGI(TAG, "LVGL display %d created (async double-buffer)", i);
    }
}

/* ================================================================== */
/*  测试画面（DISP_TEST_MODE=1 时使用，完全不依赖 LVGL）                  */
/* ================================================================== */
/*
 * 用途：一次性把「驱动通没通、颜色对不对、方向正不正」三件事全部看出来。
 *   1) 纯色 R/G/B/W/K 轮播  —— 看是否点亮、是否反色、红蓝是否颠倒
 *   2) 8 色彩条             —— 看颜色顺序和渐变是否正常
 *   3) 边框 + 四角彩色方块  —— 看左右/上下是否镜像或翻转
 * 所有数据直接写 panel，绕开 LVGL，避免页面逻辑干扰判断。
 */

/* 颜色字节序：panel 配置为 DATA_ENDIAN_BIG，而 C 里的 0xF800 是主机小端存放，
 * 所以写屏前要换一次字节。若出现紫/绿噪点这类怪异颜色，把宏改成 0 再试。 */
static inline uint16_t disp_be16(uint16_t c)
{
#if DISP_TEST_SWAP_BYTES
    return (uint16_t)((c >> 8) | (c << 8));
#else
    return c;
#endif
}

/* 阻塞等这一块传完，才能安全复用同一块缓冲 */
static inline void test_wait_done(display_dev_t *dev)
{
    if (dev->trans_done_sem) {
        xSemaphoreTake(dev->trans_done_sem, pdMS_TO_TICKS(1000));
    }
}

/* 整屏填充单一颜色 */
static void test_fill(display_dev_t *dev, uint16_t rgb565)
{
    uint16_t *buf = dev->chunk_buf[0];
    uint16_t color = disp_be16(rgb565);

    for (int y = 0; y < LCD_V_RES; y += CHUNK_LINES) {
        int h = (y + CHUNK_LINES <= LCD_V_RES) ? CHUNK_LINES : (LCD_V_RES - y);
        int n = LCD_H_RES * h;

        for (int i = 0; i < n; i++) {
            buf[i] = color;
        }

        esp_lcd_panel_draw_bitmap(dev->panel, 0, y, LCD_H_RES, y + h, buf);
        test_wait_done(dev);
    }
}

/* 8 条竖彩条：白 黄 青 绿 品红 红 蓝 黑 */
static void test_bars(display_dev_t *dev)
{
    static const uint16_t bars[8] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0,
        0xF81F, 0xF800, 0x001F, 0x0000,
    };

    uint16_t *buf = dev->chunk_buf[0];

    for (int y = 0; y < LCD_V_RES; y += CHUNK_LINES) {
        int h = (y + CHUNK_LINES <= LCD_V_RES) ? CHUNK_LINES : (LCD_V_RES - y);

        for (int ry = 0; ry < h; ry++) {
            for (int x = 0; x < LCD_H_RES; x++) {
                int bi = (x * 8) / LCD_H_RES;
                buf[ry * LCD_H_RES + x] = disp_be16(bars[bi]);
            }
        }

        esp_lcd_panel_draw_bitmap(dev->panel, 0, y, LCD_H_RES, y + h, buf);
        test_wait_done(dev);
    }
}

/* 黑底白色边框 + 四角不同颜色的方块（用来判断方向/镜像） */
static void test_frame(display_dev_t *dev)
{
    const int BORDER = 4;    /* 边框粗细 */
    const int CORNER = 48;   /* 角标边长 */

    const uint16_t c_bg     = disp_be16(0x0000); /* 黑 */
    const uint16_t c_border = disp_be16(0xFFFF); /* 白 */
    const uint16_t c_tl     = disp_be16(0xF800); /* 左上 红 */
    const uint16_t c_tr     = disp_be16(0x001F); /* 右上 蓝 */
    const uint16_t c_bl     = disp_be16(0x07E0); /* 左下 绿 */
    const uint16_t c_br     = disp_be16(0xF81F); /* 右下 品红 */

    uint16_t *buf = dev->chunk_buf[0];

    for (int y = 0; y < LCD_V_RES; y += CHUNK_LINES) {
        int h = (y + CHUNK_LINES <= LCD_V_RES) ? CHUNK_LINES : (LCD_V_RES - y);

        for (int ry = 0; ry < h; ry++) {
            int yy = y + ry;

            for (int x = 0; x < LCD_H_RES; x++) {
                uint16_t c = c_bg;

                if (x < BORDER || x >= LCD_H_RES - BORDER ||
                    yy < BORDER || yy >= LCD_V_RES - BORDER) {
                    c = c_border;
                } else if (x < CORNER && yy < CORNER) {
                    c = c_tl;
                } else if (x >= LCD_H_RES - CORNER && yy < CORNER) {
                    c = c_tr;
                } else if (x < CORNER && yy >= LCD_V_RES - CORNER) {
                    c = c_bl;
                } else if (x >= LCD_H_RES - CORNER && yy >= LCD_V_RES - CORNER) {
                    c = c_br;
                }

                buf[ry * LCD_H_RES + x] = c;
            }
        }

        esp_lcd_panel_draw_bitmap(dev->panel, 0, y, LCD_H_RES, y + h, buf);
        test_wait_done(dev);
    }
}

void display_test_pattern_run(void)
{
    static const struct {
        uint16_t    rgb;
        const char *name;
    } seq[] = {
        { 0xF800, "RED"   },
        { 0x07E0, "GREEN" },
        { 0x001F, "BLUE"  },
        { 0xFFFF, "WHITE" },
        { 0x0000, "BLACK" },
    };
    const int seq_n = (int)(sizeof(seq) / sizeof(seq[0]));

    ESP_LOGW(TAG, "========== 屏幕测试画面启动 ==========");
    ESP_LOGW(TAG, "顺序：纯红→纯绿→纯蓝→纯白→纯黑 → 彩条 → 边框 → 存活探测(闪屏)，一轮约 25 秒");
    ESP_LOGW(TAG, "同时输出到 %d 个座", s_test_target_n);

    while (1) {
        for (int i = 0; i < seq_n; i++) {
            ESP_LOGI(TAG, "纯色: %s", seq[i].name);
            for (int t = 0; t < s_test_target_n; t++) {
                test_fill(s_test_targets[t], seq[i].rgb);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        ESP_LOGI(TAG, "彩条");
        for (int t = 0; t < s_test_target_n; t++) {
            test_bars(s_test_targets[t]);
        }
        vTaskDelay(pdMS_TO_TICKS(3000));

        ESP_LOGI(TAG, "边框 + 四角标识（左上红/右上蓝/左下绿/右下品红）");
        for (int t = 0; t < s_test_target_n; t++) {
            test_frame(s_test_targets[t]);
        }
        vTaskDelay(pdMS_TO_TICKS(3000));

        /* ---------------------------------------------------------------
         * ★ 存活探测（诊断用）★
         * DISPOFF 会让面板整屏变白/全灭，DISPON 恢复；INVON 让整屏反色。
         * 这两条命令与像素数据无关，只要面板「供电正常 + 命令能送达」，
         * 屏幕就一定会跟着闪。目的是把「黑屏」拆成两种截然不同的故障：
         *   A. 屏幕有反应  → 供电/SPI/DC 都通，问题在初始化参数或像素格式
         *   B. 完全没反应  → 面板没电（脚位没对上）或面板已损坏
         * --------------------------------------------------------------- */
        ESP_LOGW(TAG, ">>> 探测A: DISPOFF 整屏应变白/灭");
        for (int t = 0; t < s_test_target_n; t++) {
            esp_lcd_panel_disp_on_off(s_test_targets[t]->panel, false);
        }
        vTaskDelay(pdMS_TO_TICKS(1500));

        ESP_LOGW(TAG, ">>> 探测B: DISPON 画面恢复");
        for (int t = 0; t < s_test_target_n; t++) {
            esp_lcd_panel_disp_on_off(s_test_targets[t]->panel, true);
        }
        vTaskDelay(pdMS_TO_TICKS(1500));

        ESP_LOGW(TAG, ">>> 探测C: INVON 整屏反色（黑↔白）");
        for (int t = 0; t < s_test_target_n; t++) {
            esp_lcd_panel_invert_color(s_test_targets[t]->panel, true);
        }
        vTaskDelay(pdMS_TO_TICKS(1500));

        ESP_LOGW(TAG, ">>> 探测D: INVOFF 恢复");
        for (int t = 0; t < s_test_target_n; t++) {
            esp_lcd_panel_invert_color(s_test_targets[t]->panel, false);
        }
        vTaskDelay(pdMS_TO_TICKS(1500));

        ESP_LOGW(TAG, ">>> 探测E: 快闪 3 次（DISP OFF/ON 各 300ms，最容易用眼睛捕捉）");
        for (int k = 0; k < 3; k++) {
            for (int t = 0; t < s_test_target_n; t++) {
                esp_lcd_panel_disp_on_off(s_test_targets[t]->panel, false);
            }
            vTaskDelay(pdMS_TO_TICKS(300));
            for (int t = 0; t < s_test_target_n; t++) {
                esp_lcd_panel_disp_on_off(s_test_targets[t]->panel, true);
            }
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}