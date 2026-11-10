#ifndef APP_BLE_CONTROL_H
#define APP_BLE_CONTROL_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化 BLE 控制模块（Peripheral 广播 + 指令通道） */
esp_err_t app_ble_control_start(void);

/** @brief 手动启动广播 */
void app_ble_start_advertising(void);

/** @brief Peripheral 是否已连接 */
bool app_ble_is_connected(void);

/** @brief 通过 Notify 发送数据 */
void app_ble_send_data(const uint8_t *data, uint16_t len);

/** @brief 关闭 BLE（幂等：已关闭时直接返回） */
void app_ble_control_stop(void);

/** @brief BLE 是否已启用 */
bool app_ble_control_is_enabled(void);

/* ==================== Central 扫描 ==================== */

/**
 * @brief 开始扫描 Xbox 蓝牙设备并自动连接
 *        BLE 需先通过 app_ble_control_start() 初始化
 */
esp_err_t app_ble_scan_xbox(void);

/** @brief 停止扫描 */
void app_ble_stop_scan(void);

/** @brief 是否正在扫描 */
bool app_ble_is_scanning(void);

/** @brief Xbox 设备是否已连接 (Central 角色) */
bool app_ble_xbox_is_connected(void);

/* ==================== 扫描管理任务 ==================== */

/**
 * @brief 外部触发接口：开启蓝牙并启动 Xbox 扫描。
 *
 *  - 若管理任务已在运行，重复调用无效（幂等）。
 *  - 15 s 内未建立连接 → 自动关闭蓝牙并销毁管理任务。
 *  - 连接成功后设备断开   → 自动关闭蓝牙并销毁管理任务。
 */
void app_ble_trigger_xbox_scan(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_BLE_CONTROL_H */