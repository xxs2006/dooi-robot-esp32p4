#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 全部可调参数 ────────────────────────────────────────── */
#define TB_LEFT_GPIO      7
#define TB_RIGHT_GPIO     8
#define TB_KEY_NUM        2
#define TB_DEFAULT_SENS   0.007f   /* 灵敏度默认值，越小越灵敏   */
#define TB_LONG_MS        900u     /* 长按触发阈值 ms            */
#define TB_DBL_MS         350u     /* 双击判定窗口 ms            */
#define TB_POLL_MS        10u      /* 轮询间隔 ms（≤10ms对人无感）*/
#define TB_INIT_SCANS     5        /* 校准扫描次数               */
#define TB_ONESHOT_MS     2000u    /* 单次扫描超时 ms            */
#define TB_CHAN_FIRST      (TOUCH_MIN_CHAN_ID + 5)  /* GPIO7/8 通道范围 */
#define TB_CHAN_LAST       (TOUCH_MIN_CHAN_ID + 6)

typedef enum {
    TOUCH_BTN_KEY_LEFT  = 0,
    TOUCH_BTN_KEY_RIGHT = 1,
    TOUCH_BTN_KEY_MAX,
} touch_btn_key_id_t;

typedef enum {
    TOUCH_BTN_EVT_CLICK        = 0,
    TOUCH_BTN_EVT_DOUBLE_CLICK,
    TOUCH_BTN_EVT_LONG_PRESS,
    TOUCH_BTN_EVT_MAX,
} touch_btn_event_t;

/**
 * @brief 触摸计数结构体
 *        记录每个按键每种事件的累计触发次数
 */
typedef struct {
    uint8_t click[TB_KEY_NUM];         /* 单击计数   */
    uint8_t double_click[TB_KEY_NUM];  /* 双击计数   */
    uint8_t long_press[TB_KEY_NUM];    /* 长按计数   */
    uint8_t total[TB_KEY_NUM];         /* 总触摸次数 (所有事件之和) */
} touch_btn_counts_t;

typedef void (*touch_btn_cb_t)(int key_id, touch_btn_event_t event, void *user_ctx);

esp_err_t touch_button_init(void);
esp_err_t touch_button_deinit(void);
esp_err_t touch_button_start(void);
esp_err_t touch_button_stop(void);
esp_err_t touch_button_register_callback(touch_btn_event_t event,
                                          touch_btn_cb_t cb, void *user_ctx);
esp_err_t touch_button_set_sensitivity(float ratio);
esp_err_t touch_button_get_sensitivity(float *ratio);

/**
 * @brief 获取触摸计数
 * @param[out] counts  填充计数结构体
 * @return ESP_OK / ESP_ERR_INVALID_ARG
 */
esp_err_t touch_button_get_counts(touch_btn_counts_t *counts);

/**
 * @brief 清零所有触摸计数
 */
esp_err_t touch_button_reset_counts(void);

#ifdef __cplusplus
}
#endif