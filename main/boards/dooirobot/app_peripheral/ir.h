/*
 * ir.h
 * 防跌落(IR)传感器驱动
 */

#ifndef _IR_H_
#define _IR_H_

#include "esp_err.h"
#include "driver/gpio.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ================= 用户配置区域 =================

#define IR_GPIO_SENSOR_1    GPIO_NUM_2
#define IR_GPIO_SENSOR_2    GPIO_NUM_3
#define IR_GPIO_SENSOR_3    GPIO_NUM_4
#define IR_GPIO_SENSOR_4    GPIO_NUM_5

#define IR_GPIO_ENABLE      6

#define IR_INTR_TYPE        GPIO_INTR_NEGEDGE
#define IR_PULL_UP_EN       1
#define IR_PULL_DOWN_EN     0

// ==============================================

typedef enum {
    IR_SENSOR_1 = 0,
    IR_SENSOR_2 = 1,
    IR_SENSOR_3 = 2,
    IR_SENSOR_4 = 3,
    IR_SENSOR_NUM = 4
} ir_sensor_id_t;

typedef void (*ir_stop_cb_t)(ir_sensor_id_t sensor_id);

/**
 * @brief 初始化 IR 模块。
 *        自动从 NVS 读取功能使能状态：
 *          - 使能：完整初始化传感器 GPIO + ISR，并开启电源
 *          - 禁用：仅配置使能引脚并保持电源关闭，跳过传感器初始化
 */
esp_err_t ir_init(ir_stop_cb_t stop_cb);

/* ----------------------------------------------------------------
 *  电源控制接口（原有，保留）
 * ---------------------------------------------------------------- */

/** @brief 开启 IR 发射管电源（使能引脚置 1） */
void ir_enable(void);

/** @brief 关闭 IR 发射管电源（使能引脚置 0） */
void ir_disable(void);

/* ----------------------------------------------------------------
 *  ISR 静音接口（原有，保留）
 * ---------------------------------------------------------------- */

void ir_mute(void);
void ir_unmute(void);

/* ----------------------------------------------------------------
 *  功能使能控制接口（新增，操作持久化到 NVS）
 * ---------------------------------------------------------------- */

/**
 * @brief 使能 IR 功能，并持久化到 NVS。
 *        - 若传感器从未初始化，则补充执行 GPIO + ISR 安装
 *        - 恢复 ISR（如处于静音状态）
 *        - 开启电源
 */
esp_err_t ir_feature_enable(void);

/**
 * @brief 禁用 IR 功能，并持久化到 NVS。
 *        - 关闭电源
 *        - 移除 ISR handlers（静音）
 */
esp_err_t ir_feature_disable(void);

/**
 * @brief 查询当前 IR 功能是否使能（运行时状态，非 NVS 读取）
 */
bool ir_feature_is_enabled(void);

/* ----------------------------------------------------------------
 *  统计接口
 * ---------------------------------------------------------------- */

void ir_trigger_count_get(ir_sensor_id_t sensor_id, uint8_t *count);

#ifdef __cplusplus
}
#endif

#endif // _IR_H_