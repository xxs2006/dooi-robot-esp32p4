/*
 * ir.c
 * 防跌落(IR)传感器驱动实现
 */

#include "ir.h"
#include "dooi_nvs.h"
#include "esp_attr.h"
#include "esp_log.h"

static const char *TAG = "IR_MOD";

static ir_stop_cb_t s_stop_func        = NULL;
static bool         s_ir_is_mute       = false;
static bool         s_ir_feature_en    = true;   // 功能使能状态（从 NVS 读取）
static bool         s_ir_hw_inited     = false;  // 传感器 GPIO + ISR 是否已安装
static uint8_t      s_ir_trigger_count[IR_SENSOR_NUM] = {0};

/* ----------------------------------------------------------------
 *  中断服务程序
 * ---------------------------------------------------------------- */
static void IRAM_ATTR ir_sensor_isr_handler(void *arg)
{
    ir_sensor_id_t sensor_id = (ir_sensor_id_t)(uintptr_t)arg;
    s_ir_trigger_count[sensor_id]++;
    if (s_stop_func) {
        s_stop_func(sensor_id);
    }
}

/* ----------------------------------------------------------------
 *  内部：传感器 GPIO + ISR 安装（幂等，只执行一次）
 * ---------------------------------------------------------------- */
static esp_err_t ir_hw_init(void)
{
    if (s_ir_hw_inited) {
        return ESP_OK;
    }

    esp_err_t ret;

    /* 1. 配置 4 个传感器输入引脚 */
    uint64_t sensor_mask = (1ULL << IR_GPIO_SENSOR_1) |
                           (1ULL << IR_GPIO_SENSOR_2) |
                           (1ULL << IR_GPIO_SENSOR_3) |
                           (1ULL << IR_GPIO_SENSOR_4);

    gpio_config_t io_conf_in = {
        .intr_type    = IR_INTR_TYPE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = sensor_mask,
        .pull_up_en   = IR_PULL_UP_EN,
        .pull_down_en = IR_PULL_DOWN_EN
    };
    ret = gpio_config(&io_conf_in);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Sensor GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2. 安装 ISR 服务（ESP_INTR_FLAG_IRAM 允许 Cache 禁用时运行） */
    ret = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Install ISR service failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 3. 注册 ISR 处理函数 */
    gpio_isr_handler_add(IR_GPIO_SENSOR_1, ir_sensor_isr_handler,
                         (void *)(uintptr_t)IR_SENSOR_1);
    gpio_isr_handler_add(IR_GPIO_SENSOR_2, ir_sensor_isr_handler,
                         (void *)(uintptr_t)IR_SENSOR_2);
    gpio_isr_handler_add(IR_GPIO_SENSOR_3, ir_sensor_isr_handler,
                         (void *)(uintptr_t)IR_SENSOR_3);
    gpio_isr_handler_add(IR_GPIO_SENSOR_4, ir_sensor_isr_handler,
                         (void *)(uintptr_t)IR_SENSOR_4);

    s_ir_hw_inited = true;
    s_ir_is_mute   = false;
    ESP_LOGI(TAG, "IR HW (GPIO + ISR) initialized");
    return ESP_OK;
}

/* ================================================================
 *  公共接口实现
 * ================================================================ */

esp_err_t ir_init(ir_stop_cb_t stop_cb)
{
    esp_err_t ret;
    s_stop_func = stop_cb;

    /* 1. 配置 IR 使能引脚（输出，驱动发射管） */
    gpio_config_t io_conf_out = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << IR_GPIO_ENABLE),
        .pull_down_en = 0,
        .pull_up_en   = 0
    };
    ret = gpio_config(&io_conf_out);
    if (ret != ESP_OK) return ret;

    /* 2. 从 NVS 读取功能使能状态（读取失败时默认使能） */
    bool feature_en = true;
    dooi_nvs_get_ir_enable(&feature_en);
    s_ir_feature_en = feature_en;

    /* 3. 按状态决定初始化深度 */
    if (s_ir_feature_en) {
        ret = ir_hw_init();   // 传感器 GPIO + ISR
        if (ret != ESP_OK) return ret;
        ir_enable();          // 电源开
        ESP_LOGI(TAG, "IR initialized, power ON");
    } else {
        ir_disable();         // 确保电源关闭，跳过传感器初始化
        ESP_LOGI(TAG, "IR disabled by NVS — power OFF, ISR not installed");
    }

    return ESP_OK;
}

/* ---- 电源控制（原有） ---- */

void ir_enable(void)
{
    if(!s_ir_feature_en) return;
    gpio_set_level(IR_GPIO_ENABLE, 1);
}

void ir_disable(void)
{
    gpio_set_level(IR_GPIO_ENABLE, 0);
}

/* ---- ISR 静音（原有） ---- */

void ir_mute(void)
{
    if(!s_ir_feature_en) return;
    if (s_ir_is_mute) return;
    s_ir_is_mute = true;
    gpio_isr_handler_remove(IR_GPIO_SENSOR_1);
    gpio_isr_handler_remove(IR_GPIO_SENSOR_2);
    gpio_isr_handler_remove(IR_GPIO_SENSOR_3);
    gpio_isr_handler_remove(IR_GPIO_SENSOR_4);
}

void ir_unmute(void)
{
    if (!s_ir_is_mute) return;
    s_ir_is_mute = false;
    gpio_isr_handler_add(IR_GPIO_SENSOR_1, ir_sensor_isr_handler,
                         (void *)(uintptr_t)IR_SENSOR_1);
    gpio_isr_handler_add(IR_GPIO_SENSOR_2, ir_sensor_isr_handler,
                         (void *)(uintptr_t)IR_SENSOR_2);
    gpio_isr_handler_add(IR_GPIO_SENSOR_3, ir_sensor_isr_handler,
                         (void *)(uintptr_t)IR_SENSOR_3);
    gpio_isr_handler_add(IR_GPIO_SENSOR_4, ir_sensor_isr_handler,
                         (void *)(uintptr_t)IR_SENSOR_4);
}

/* ---- 功能使能控制（新增，持久化）---- */

esp_err_t ir_feature_enable(void)
{
    s_ir_feature_en = true;

    if (!s_ir_hw_inited) {
        /* 首次使能：完整安装传感器 GPIO + ISR */
        esp_err_t ret = ir_hw_init();
        if (ret != ESP_OK) {
            s_ir_feature_en = false;
            ESP_LOGE(TAG, "ir_hw_init failed: %s", esp_err_to_name(ret));
            return ret;
        }
    } else {
        /* 已初始化：恢复 ISR（如处于静音状态） */
        ir_unmute();
    }

    ir_enable();  /* 电源开 */

    esp_err_t ret = dooi_nvs_set_ir_enable(true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS persist failed: %s", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "IR feature ENABLED (persisted=%s)",
             ret == ESP_OK ? "OK" : "FAIL");
    return ret;
}

esp_err_t ir_feature_disable(void)
{
    s_ir_feature_en = false;

    ir_disable();               /* 电源关 */

    if (s_ir_hw_inited) {
        ir_mute();              /* 移除 ISR handlers */
    }

    esp_err_t ret = dooi_nvs_set_ir_enable(false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS persist failed: %s", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "IR feature DISABLED (persisted=%s)",
             ret == ESP_OK ? "OK" : "FAIL");
    return ret;
}

bool ir_feature_is_enabled(void)
{
    return s_ir_feature_en;
}

/* ---- 统计 ---- */

void ir_trigger_count_get(ir_sensor_id_t sensor_id, uint8_t *count)
{
    if (sensor_id >= IR_SENSOR_NUM) {
        ESP_LOGE(TAG, "Invalid sensor ID");
        return;
    }
    *count = s_ir_trigger_count[sensor_id];
}