#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POWER_BUTTON_PIN GPIO_NUM_49

/* 软件电源控制脚：拉高开机，拉低关机 */
#define POWER_CTRL_PIN   GPIO_NUM_50

typedef void (*power_charging_cb_t)(bool is_charging);
typedef void (*power_low_battery_cb_t)(bool is_low_battery);
typedef void (*power_btn_cb_t)(void);
typedef void (*power_shutdown_cb_t)(void);

typedef struct {
    power_charging_cb_t    on_charging_changed;
    power_low_battery_cb_t on_low_battery_changed;
    power_btn_cb_t         on_power_btn_pressed;
    power_shutdown_cb_t    on_power_shutdown;
} power_config_t;

esp_err_t power_manager_init(const power_config_t *config);
uint8_t   power_manager_get_level(void);
bool      power_manager_is_charging(void);
bool      power_manager_is_discharging(void);
bool      power_manager_is_low_battery(void);
void      power_manager_pause(void);
void      power_manager_resume(void);
void      power_manager_deinit(void);

/**
 * @brief 执行开机：拉高 POWER_CTRL_PIN，保持整机供电
 */
void power_manager_power_on(void);

/**
 * @brief 执行关机：拉低 POWER_CTRL_PIN，切断整机电源
 */
void power_manager_power_off(void);

#ifdef __cplusplus
}
#endif

#endif /* POWER_MANAGER_H */