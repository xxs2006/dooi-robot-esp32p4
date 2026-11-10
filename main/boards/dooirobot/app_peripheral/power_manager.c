#include "power_manager.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "PowerMgr";

/* ========== 硬件配置 ========== */
#define POWER_ADC_UNIT      ADC_UNIT_2
#define POWER_ADC_CHANNEL   ADC_CHANNEL_5
#define POWER_CHARGE_PIN    GPIO_NUM_53

/* ========== 电源按键防抖（时间戳方式） ========== */
#define POWER_BTN_DEBOUNCE_US   50000LL   /* 50 ms */
#define POWER_LONG_PRESS_US     1500000LL /* 1.5s 长按开关机 */

/* ========== 业务逻辑常量 ========== */
#define BATTERY_ADC_DATA_COUNT   3
#define BATTERY_ADC_INTERVAL     60
#define LOW_BATTERY_LEVEL        15
#define TIMER_PERIOD_US          10000000

/* ========== 电池电量映射表 ========== */
typedef struct {
    uint16_t adc;
    uint8_t  level;
} battery_map_t;

static const battery_map_t BATTERY_LEVELS[] = {
    {1650,   0},
    {1750,  10},
    {1825,  20},
    {1900,  40},
    {1975,  60},
    {2050,  80},
    {2100, 100},
};

#define MAP_SIZE (sizeof(BATTERY_LEVELS) / sizeof(BATTERY_LEVELS[0]))

/* ========== 模块内部状态 ========== */
typedef struct {
    adc_oneshot_unit_handle_t adc_handle;
    esp_timer_handle_t        timer;

    power_charging_cb_t       on_charging_changed;
    power_low_battery_cb_t    on_low_battery_changed;
    power_btn_cb_t            on_power_btn_pressed;
    power_shutdown_cb_t       on_power_shutdown;

    esp_timer_handle_t        long_press_timer;   /* 长按计时 */
    uint64_t                  press_start_tick;   /* 按下时刻 */

    uint16_t adc_values[BATTERY_ADC_DATA_COUNT];
    int      adc_count;
    uint32_t battery_level;
    bool     is_charging;
    bool     is_low_battery;
    int      ticks;
    bool     paused;
    bool     initialized;
} power_mgr_t;

static power_mgr_t s_mgr = {0};

/* ================================================================
 *  内部函数：滑动窗口
 * ================================================================ */
static void adc_window_push(uint16_t value)
{
    if (s_mgr.adc_count < BATTERY_ADC_DATA_COUNT) {
        s_mgr.adc_values[s_mgr.adc_count++] = value;
    } else {
        for (int i = 0; i < BATTERY_ADC_DATA_COUNT - 1; i++) {
            s_mgr.adc_values[i] = s_mgr.adc_values[i + 1];
        }
        s_mgr.adc_values[BATTERY_ADC_DATA_COUNT - 1] = value;
    }
}

static uint32_t adc_window_average(void)
{
    if (s_mgr.adc_count == 0) return 0;
    uint32_t sum = 0;
    for (int i = 0; i < s_mgr.adc_count; i++) sum += s_mgr.adc_values[i];
    return sum / (uint32_t)s_mgr.adc_count;
}

/* ================================================================
 *  内部函数：分段线性插值
 * ================================================================ */
static uint8_t calc_battery_level(uint32_t adc_avg)
{
    if (adc_avg < BATTERY_LEVELS[0].adc) return 0;
    if (adc_avg >= BATTERY_LEVELS[MAP_SIZE - 1].adc) return 100;

    for (int i = 0; i < (int)(MAP_SIZE - 1); i++) {
        if (adc_avg >= BATTERY_LEVELS[i].adc && adc_avg < BATTERY_LEVELS[i + 1].adc) {
            float ratio = (float)(adc_avg - BATTERY_LEVELS[i].adc) /
                          (float)(BATTERY_LEVELS[i + 1].adc - BATTERY_LEVELS[i].adc);
            return BATTERY_LEVELS[i].level +
                   (uint8_t)(ratio * (float)(BATTERY_LEVELS[i + 1].level - BATTERY_LEVELS[i].level));
        }
    }
    return 100;
}

/* ================================================================
 *  内部函数：ADC 采样
 * ================================================================ */
static void read_battery_adc_data(void)
{
    int adc_value = 0;
    esp_err_t ret = adc_oneshot_read(s_mgr.adc_handle, POWER_ADC_CHANNEL, &adc_value);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ADC read failed: %s", esp_err_to_name(ret));
        return;
    }

    adc_window_push((uint16_t)adc_value);
    uint32_t average_adc = adc_window_average();
    s_mgr.battery_level  = calc_battery_level(average_adc);

    if (s_mgr.adc_count >= BATTERY_ADC_DATA_COUNT) {
        bool new_low = (s_mgr.battery_level <= LOW_BATTERY_LEVEL);
        if (new_low != s_mgr.is_low_battery) {
            s_mgr.is_low_battery = new_low;
            if (s_mgr.on_low_battery_changed) {
                s_mgr.on_low_battery_changed(s_mgr.is_low_battery);
            }
        }
    }

    ESP_LOGI(TAG, "ADC value: %d, average: %lu, level: %lu%%",
             adc_value, (unsigned long)average_adc, (unsigned long)s_mgr.battery_level);
}

/* ================================================================
 *  内部函数：定时器回调
 * ================================================================ */
static void power_timer_callback(void *arg)
{
    (void)arg;
    if (s_mgr.paused) return;

    bool new_charging = (gpio_get_level(POWER_CHARGE_PIN) == 0);
    if (new_charging != s_mgr.is_charging) {
        s_mgr.is_charging = new_charging;
        if (s_mgr.on_charging_changed) s_mgr.on_charging_changed(s_mgr.is_charging);
        read_battery_adc_data();
        return;
    }

    if (s_mgr.adc_count < BATTERY_ADC_DATA_COUNT) {
        read_battery_adc_data();
        return;
    }

    s_mgr.ticks++;
    if (s_mgr.ticks % BATTERY_ADC_INTERVAL == 0) {
        read_battery_adc_data();
    }
}

/* ================================================================
 *  长按计时回调：按下持续达到 3s 即触发关机
 * ================================================================ */
static void power_long_press_cb(void *arg)
{
    (void)arg;
    /* 计时到期时若按键仍被按住（低电平），则触发关机 */
    if (gpio_get_level(POWER_BUTTON_PIN) == 0) {
        ESP_LOGW(TAG, "Power button long-pressed 3s, requesting shutdown...");
        if (s_mgr.on_power_shutdown) {
            s_mgr.on_power_shutdown();
        }
    }
}

/* ================================================================
 *  电源按键 ISR（带时间戳防抖，处理按下/松开）
 * ================================================================ */
static void IRAM_ATTR power_btn_isr_handler(void *arg)
{
    static uint64_t s_last_btn_tick = 0;
    uint64_t now = esp_timer_get_time();

    /* 去抖：忽略 POWER_BTN_DEBOUNCE_US 内的重复触发 */
    if (now - s_last_btn_tick < POWER_BTN_DEBOUNCE_US) {
        return;
    }
    s_last_btn_tick = now;

    if (gpio_get_level(POWER_BUTTON_PIN) == 0) {
        /* ── 按下：记录时刻，启动长按计时，上报单击 ── */
        s_mgr.press_start_tick = now;
        if (s_mgr.long_press_timer) {
            esp_timer_stop(s_mgr.long_press_timer);
            esp_timer_start_once(s_mgr.long_press_timer, POWER_LONG_PRESS_US);
        }
        if (s_mgr.on_power_btn_pressed) {
            s_mgr.on_power_btn_pressed();
        }
    } else {
        /* ── 松开：取消长按计时 ── */
        if (s_mgr.long_press_timer) {
            esp_timer_stop(s_mgr.long_press_timer);
        }
    }
}

/* ================================================================
 *  公开 API
 * ================================================================ */
esp_err_t power_manager_init(const power_config_t *config)
{
    if (s_mgr.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_mgr, 0, sizeof(power_mgr_t));

    /* ---------- 充电检测引脚 ---------- */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << POWER_CHARGE_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    /* ---------- ADC ---------- */
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id  = POWER_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &s_mgr.adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_mgr.adc_handle, POWER_ADC_CHANNEL, &chan_cfg));

    /* ---------- 保存回调 ---------- */
    if (config) {
        s_mgr.on_charging_changed    = config->on_charging_changed;
        s_mgr.on_low_battery_changed = config->on_low_battery_changed;
        s_mgr.on_power_btn_pressed   = config->on_power_btn_pressed;
        s_mgr.on_power_shutdown      = config->on_power_shutdown;
    }

    /* ---------- 软件电源控制脚（拉高开机） ---------- */
    gpio_config_t pwr_ctrl_conf = {
        .pin_bit_mask = (1ULL << POWER_CTRL_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pwr_ctrl_conf));
    gpio_set_level(POWER_CTRL_PIN, 1);   /* 拉高，保持整机供电 */

    /* ---------- 初始状态 ---------- */
    s_mgr.battery_level = 80;
    s_mgr.is_charging   = (gpio_get_level(POWER_CHARGE_PIN) == 0);

    /* ---------- 首次采样 ---------- */
    read_battery_adc_data();

    /* ---------- 定时器 ---------- */
    const esp_timer_create_args_t timer_args = {
        .callback              = power_timer_callback,
        .arg                   = NULL,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "battery_check_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_mgr.timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_mgr.timer, TIMER_PERIOD_US));

    /* ---------- 长按计时器 ---------- */
    const esp_timer_create_args_t long_press_args = {
        .callback              = power_long_press_cb,
        .arg                   = NULL,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "power_long_press",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&long_press_args, &s_mgr.long_press_timer));

    /* ---------- 电源按键：双边沿，按下/松开都触发 ---------- */
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << POWER_BUTTON_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,    /* 按下/松开都触发 */
    };
    ESP_ERROR_CHECK(gpio_config(&btn_conf));

    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(isr_ret);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(POWER_BUTTON_PIN, power_btn_isr_handler, NULL));

    s_mgr.initialized = true;
    ESP_LOGI(TAG, "Initialized. Charging: %s, Level: %lu%%",
             s_mgr.is_charging ? "YES" : "NO", (unsigned long)s_mgr.battery_level);

    return ESP_OK;
}

uint8_t power_manager_get_level(void)
{
    return (uint8_t)s_mgr.battery_level;
}

bool power_manager_is_charging(void)
{
    return s_mgr.is_charging;
}

bool power_manager_is_discharging(void) { return !s_mgr.is_charging; }
bool power_manager_is_low_battery(void) { return s_mgr.is_low_battery; }
void power_manager_pause(void)          { s_mgr.paused = true;  ESP_LOGI(TAG, "Sampling paused"); }
void power_manager_resume(void)         { s_mgr.paused = false; ESP_LOGI(TAG, "Sampling resumed"); }

void power_manager_power_on(void)
{
    ESP_LOGI(TAG, "Power on: pull GPIO%d HIGH...", POWER_CTRL_PIN);
    gpio_set_level(POWER_CTRL_PIN, 1);   /* 拉高，保持整机供电 */
}

void power_manager_power_off(void)
{
    ESP_LOGW(TAG, "Power off: pull GPIO%d LOW...", POWER_CTRL_PIN);
    gpio_set_level(POWER_CTRL_PIN, 0);   /* 拉低，切断整机电源 */
}

void power_manager_deinit(void)
{
    if (!s_mgr.initialized) return;

    gpio_isr_handler_remove(POWER_BUTTON_PIN);

    if (s_mgr.long_press_timer) {
        esp_timer_stop(s_mgr.long_press_timer);
        esp_timer_delete(s_mgr.long_press_timer);
        s_mgr.long_press_timer = NULL;
    }

    if (s_mgr.timer) {
        esp_timer_stop(s_mgr.timer);
        esp_timer_delete(s_mgr.timer);
    }

    if (s_mgr.adc_handle) {
        adc_oneshot_del_unit(s_mgr.adc_handle);
    }

    memset(&s_mgr, 0, sizeof(power_mgr_t));
    ESP_LOGI(TAG, "Deinitialized");
}