#include "touch_button.h"
#include "dooi_nvs.h"

#include <string.h>
#include <inttypes.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/touch_sens.h"

static const char *TAG = "touch_btn";

/* ── 内部类型（3个，最小化）────────────────────────────── */

typedef struct {
    volatile bool  active;      /* 硬件当前是否被触摸       */
    bool           prev;        /* 上次轮询的 active 值     */
    bool           long_fired;  /* 长按已派发，防止重复触发 */
    uint8_t        click_cnt;   /* 待判定的点击次数         */
    uint32_t       press_ms;    /* 持续按下时间             */
    uint32_t       rel_ms;      /* 释放后经过时间           */
} key_state_t;

typedef struct { touch_btn_cb_t cb; void *user_ctx; } cb_slot_t;

/* ── 单例（全静态，零堆分配）───────────────────────────── */
static struct {
    float                  sens_ratio;
    touch_sensor_handle_t  hw_sens;
    touch_channel_handle_t chans[TB_KEY_NUM];
    int                    chan_ids[TB_KEY_NUM];
    esp_timer_handle_t     poll_timer;
    key_state_t            ks[TB_KEY_NUM];
    cb_slot_t              subs[TOUCH_BTN_EVT_MAX];
    touch_btn_counts_t     counts;       /* ← 新增: 触摸计数 */
    bool                   started;
    bool                   initialized;
} s;

static const int k_gpios[TB_KEY_NUM] = { TB_LEFT_GPIO, TB_RIGHT_GPIO };

/* ── 事件分发 ───────────────────────────────────────────── */
static inline void dispatch(int idx, touch_btn_event_t evt)
{
    /* 累加计数器 */
    switch (evt) {
    case TOUCH_BTN_EVT_CLICK:
        s.counts.click[idx]++;
        break;
    case TOUCH_BTN_EVT_DOUBLE_CLICK:
        s.counts.double_click[idx]++;
        break;
    case TOUCH_BTN_EVT_LONG_PRESS:
        s.counts.long_press[idx]++;
        break;
    default:
        break;
    }
    s.counts.total[idx]++;

    const cb_slot_t *c = &s.subs[evt];
    if (c->cb) c->cb(idx, evt, c->user_ctx);
}

/* ── ISR：极轻量，只写一个 bool ────────────────────────── */
static bool IRAM_ATTR on_active(touch_sensor_handle_t h,
                                 const touch_active_event_data_t *e, void *ctx)
{
    for (int i = 0; i < TB_KEY_NUM; i++)
        if (s.chan_ids[i] == (int)e->chan_id) { s.ks[i].active = true; break; }
    return false;
}

static bool IRAM_ATTR on_inactive(touch_sensor_handle_t h,
                                   const touch_inactive_event_data_t *e, void *ctx)
{
    for (int i = 0; i < TB_KEY_NUM; i++)
        if (s.chan_ids[i] == (int)e->chan_id) { s.ks[i].active = false; break; }
    return false;
}

/* ── 轮询状态机（每 10ms，运行于 esp_timer_task）──────── */
static void poll_cb(void *arg)
{
    for (int i = 0; i < TB_KEY_NUM; i++) {
        key_state_t *k = &s.ks[i];
        const bool act = k->active;

        if (act && !k->prev) {                            /* ① 按下 */
            k->prev       = true;
            k->press_ms   = 0;
            k->long_fired = false;

        } else if (!act && k->prev) {                     /* ② 释放 */
            k->prev = false;
            if (!k->long_fired) { k->click_cnt++; k->rel_ms = 0; }

        } else if (act && !k->long_fired) {               /* ③ 长按计时 */
            if ((k->press_ms += TB_POLL_MS) >= TB_LONG_MS) {
                k->long_fired = true;
                dispatch(i, TOUCH_BTN_EVT_LONG_PRESS);
            }

        } else if (!act && k->click_cnt) {                /* ④ 双击窗口 */
            if ((k->rel_ms += TB_POLL_MS) >= TB_DBL_MS) {
                dispatch(i, k->click_cnt >= 2
                            ? TOUCH_BTN_EVT_DOUBLE_CLICK : TOUCH_BTN_EVT_CLICK);
                k->click_cnt = 0;
            }
        }
    }
}

/* ── OneShot 校准 ───────────────────────────────────────── */
static esp_err_t do_calibrate(void)
{
    ESP_RETURN_ON_ERROR(touch_sensor_enable(s.hw_sens),  TAG, "enable");
    for (int i = 0; i < TB_INIT_SCANS; i++)
        ESP_RETURN_ON_ERROR(
            touch_sensor_trigger_oneshot_scanning(s.hw_sens, TB_ONESHOT_MS),
            TAG, "scan[%d]", i);
    ESP_RETURN_ON_ERROR(touch_sensor_disable(s.hw_sens), TAG, "disable");

    for (int i = 0; i < TB_KEY_NUM; i++) {
        uint32_t bm[TOUCH_SAMPLE_CFG_NUM] = {0};
        ESP_RETURN_ON_ERROR(touch_channel_read_data(s.chans[i],
#if SOC_TOUCH_SUPPORT_BENCHMARK
            TOUCH_CHAN_DATA_TYPE_BENCHMARK,
#else
            TOUCH_CHAN_DATA_TYPE_SMOOTH,
#endif
            bm), TAG, "read[%d]", i);

        touch_channel_config_t cfg = {0};
#if SOC_TOUCH_SENSOR_VERSION == 1
        for (int j = 0; j < TOUCH_SAMPLE_CFG_NUM; j++)
            cfg.abs_active_thresh[j] = (uint32_t)(bm[j] * (1.0f - s.sens_ratio));
        cfg.charge_speed     = TOUCH_CHARGE_SPEED_7;
        cfg.init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT;
        cfg.group            = TOUCH_CHAN_TRIG_GROUP_BOTH;
#else
        for (int j = 0; j < TOUCH_SAMPLE_CFG_NUM; j++)
            cfg.active_thresh[j] = (uint32_t)(bm[j] * s.sens_ratio);
#endif
        ESP_RETURN_ON_ERROR(
            touch_sensor_reconfig_channel(s.chans[i], &cfg), TAG, "reconfig[%d]", i);
        ESP_LOGI(TAG, "key[%d] gpio=%d bm=%"PRIu32" ratio=%.4f",
                 i, k_gpios[i], bm[0], s.sens_ratio);
    }
    return ESP_OK;
}

/* ── 通道探测 ───────────────────────────────────────────── */
static esp_err_t create_channels(void)
{
    touch_channel_config_t init = {0};
#if SOC_TOUCH_SENSOR_VERSION == 3
    init.active_thresh[0] = 1000; init.active_thresh[1] = 2500;
#elif SOC_TOUCH_SENSOR_VERSION == 2
    init.active_thresh[0] = 2000;
#else
    init.abs_active_thresh[0] = 1000;
    init.charge_speed          = TOUCH_CHARGE_SPEED_7;
    init.init_charge_volt      = TOUCH_INIT_CHARGE_VOLT_DEFAULT;
    init.group                 = TOUCH_CHAN_TRIG_GROUP_BOTH;
#endif

    bool found[TB_KEY_NUM] = {false};
    for (int cid = TB_CHAN_FIRST; cid <= TB_CHAN_LAST; cid++) {
        touch_channel_handle_t h = NULL;
        if (touch_sensor_new_channel(s.hw_sens, cid, &init, &h) != ESP_OK) continue;

        touch_chan_info_t info = {0};
        if (touch_sensor_get_channel_info(h, &info) != ESP_OK)
            { touch_sensor_del_channel(h); continue; }

        bool matched = false;
        for (int k = 0; k < TB_KEY_NUM && !matched; k++) {
            if (!found[k] && info.chan_gpio == k_gpios[k]) {
                s.chans[k] = h; s.chan_ids[k] = cid;
                found[k] = matched = true;
                ESP_LOGI(TAG, "key[%d] gpio=%d → chan=%d", k, k_gpios[k], cid);
            }
        }
        if (!matched) touch_sensor_del_channel(h);
    }

    for (int k = 0; k < TB_KEY_NUM; k++)
        ESP_RETURN_ON_FALSE(found[k], ESP_ERR_NOT_FOUND, TAG,
                            "key[%d] gpio=%d 未找到通道", k, k_gpios[k]);
    return ESP_OK;
}

/* ── 资源清理 ───────────────────────────────────────────── */
static void cleanup(void)
{
    if (s.poll_timer) {
        esp_timer_stop(s.poll_timer);
        esp_timer_delete(s.poll_timer);
    }
    for (int i = 0; i < TB_KEY_NUM; i++)
        if (s.chans[i]) touch_sensor_del_channel(s.chans[i]);
    if (s.hw_sens) touch_sensor_del_controller(s.hw_sens);
    memset(&s, 0, sizeof(s));
}

/* ── 公共 API ───────────────────────────────────────────── */
esp_err_t touch_button_init(void)
{
    if (s.initialized) return ESP_OK;
    esp_err_t ret = ESP_OK;
    memset(&s, 0, sizeof(s));
    for (int i = 0; i < TB_KEY_NUM; i++) s.chan_ids[i] = -1;

    /* ① NVS 读灵敏度 */
    s.sens_ratio = TB_DEFAULT_SENS;
    dooi_nvs_get_touch_sensitivity(&s.sens_ratio);
    ESP_LOGI(TAG, "sensitivity=%.4f", s.sens_ratio);

    /* ② Sensor controller */
    touch_sensor_sample_config_t samp[TOUCH_SAMPLE_CFG_NUM] = {
#if SOC_TOUCH_SENSOR_VERSION == 3
        TOUCH_SENSOR_V3_DEFAULT_SAMPLE_CONFIG2(3, 29, 8, 3),
        TOUCH_SENSOR_V3_DEFAULT_SAMPLE_CONFIG2(2, 88, 31, 7),
        TOUCH_SENSOR_V3_DEFAULT_SAMPLE_CONFIG2(3, 10, 31, 7),
#elif SOC_TOUCH_SENSOR_VERSION == 2
        TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_2V2),
#else
        TOUCH_SENSOR_V1_DEFAULT_SAMPLE_CONFIG(5.0, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_1V7),
#endif
    };
    touch_sensor_config_t sens_cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(
        SOC_TOUCH_SENSOR_VERSION == 3 ? TOUCH_SAMPLE_CFG_NUM : 1, samp);
    ESP_GOTO_ON_ERROR(touch_sensor_new_controller(&sens_cfg, &s.hw_sens), fail, TAG, "controller");

    /* ③ 通道 → 过滤器 → 校准 → ISR */
    ESP_GOTO_ON_ERROR(create_channels(), fail, TAG, "channels");

    touch_sensor_filter_config_t f = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    ESP_GOTO_ON_ERROR(touch_sensor_config_filter(s.hw_sens, &f), fail, TAG, "filter");

    ESP_GOTO_ON_ERROR(do_calibrate(), fail, TAG, "calibrate");

    touch_event_callbacks_t cbs = { .on_active = on_active, .on_inactive = on_inactive };
    ESP_GOTO_ON_ERROR(touch_sensor_register_callbacks(s.hw_sens, &cbs, NULL), fail, TAG, "callbacks");

    /* ④ 轮询定时器 */
    esp_timer_create_args_t ta = {
        .callback              = poll_cb,
        .dispatch_method       = ESP_TIMER_TASK,
        .skip_unhandled_events = true,
        .name                  = "tb_poll",
    };
    ESP_GOTO_ON_ERROR(esp_timer_create(&ta, &s.poll_timer), fail, TAG, "poll timer");

    s.initialized = true;
    return ESP_OK;
fail:
    cleanup();
    return ret;
}

esp_err_t touch_button_deinit(void)
{
    if (!s.initialized) return ESP_OK;
    touch_button_stop();
    cleanup();
    return ESP_OK;
}

esp_err_t touch_button_start(void)
{
    ESP_RETURN_ON_FALSE(s.initialized, ESP_ERR_INVALID_STATE, TAG, "not init");
    if (s.started) return ESP_OK;
    ESP_RETURN_ON_ERROR(touch_sensor_enable(s.hw_sens),                              TAG, "enable");
    ESP_RETURN_ON_ERROR(touch_sensor_start_continuous_scanning(s.hw_sens),           TAG, "start scan");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s.poll_timer, TB_POLL_MS * 1000ULL),TAG, "start poll");
    s.started = true;
    return ESP_OK;
}

esp_err_t touch_button_stop(void)
{
    if (!s.started) return ESP_OK;
    esp_timer_stop(s.poll_timer);
    touch_sensor_stop_continuous_scanning(s.hw_sens);
    touch_sensor_disable(s.hw_sens);
    s.started = false;
    return ESP_OK;
}

esp_err_t touch_button_register_callback(touch_btn_event_t event,
                                          touch_btn_cb_t cb, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(cb && event < TOUCH_BTN_EVT_MAX, ESP_ERR_INVALID_ARG, TAG, "args");
    s.subs[event] = (cb_slot_t){ cb, user_ctx };
    return ESP_OK;
}

esp_err_t touch_button_set_sensitivity(float ratio)
{
    ESP_RETURN_ON_FALSE(s.initialized, ESP_ERR_INVALID_STATE, TAG, "not init");
    if (ratio < 0.001f) ratio = 0.001f;
    if (ratio > 0.100f) ratio = 0.100f;

    bool was = s.started;
    if (was) ESP_RETURN_ON_ERROR(touch_button_stop(), TAG, "stop");

    s.sens_ratio = ratio;
    esp_err_t ret = do_calibrate();
    if (ret == ESP_OK) {
        dooi_nvs_set_touch_sensitivity(ratio);
        ESP_LOGI(TAG, "sensitivity → %.4f", ratio);
    }
    if (was) touch_button_start();
    return ret;
}

esp_err_t touch_button_get_sensitivity(float *ratio)
{
    ESP_RETURN_ON_FALSE(ratio, ESP_ERR_INVALID_ARG, TAG, "null");
    *ratio = s.initialized ? s.sens_ratio : TB_DEFAULT_SENS;
    return ESP_OK;
}

/* ── 触摸计数 API ──────────────────────────────────────── */
esp_err_t touch_button_get_counts(touch_btn_counts_t *counts)
{
    ESP_RETURN_ON_FALSE(counts, ESP_ERR_INVALID_ARG, TAG, "null");
    memcpy(counts, &s.counts, sizeof(touch_btn_counts_t));
    return ESP_OK;
}

esp_err_t touch_button_reset_counts(void)
{
    memset(&s.counts, 0, sizeof(touch_btn_counts_t));
    ESP_LOGI(TAG, "touch counts reset");
    return ESP_OK;
}