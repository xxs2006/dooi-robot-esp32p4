#ifndef NIMBLE_H
#define NIMBLE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 回调类型 ==================== */

/** @brief 数据接收回调（Peripheral: 对端写入 / Central: 收到 Notification） */
typedef void (*nimble_data_recv_cb_t)(uint8_t *data, uint16_t len);

/** @brief 连接状态变化回调 */
typedef void (*nimble_conn_state_cb_t)(bool connected);

/** @brief 扫描到设备回调（所有可见设备都会上报） */
typedef void (*nimble_scan_result_cb_t)(const char *name,
                                        const uint8_t *addr,
                                        uint8_t addr_type,
                                        int8_t rssi);

/* ==================== 回调配置 ==================== */

typedef struct {
    /* --- Peripheral 模式 --- */
    nimble_data_recv_cb_t   on_data_recv;       ///< 对端写入数据
    nimble_conn_state_cb_t  on_conn_state;      ///< Peripheral 连接状态

    /* --- Central 模式 (新增) --- */
    nimble_scan_result_cb_t on_scan_result;     ///< 扫描到设备（可为 NULL）
    nimble_conn_state_cb_t  on_central_conn;    ///< Central 连接状态
    nimble_data_recv_cb_t   on_central_notify;  ///< 收到远端 Notification/Indication
} nimble_callbacks_t;

/* ==================== 初始化/反初始化 ==================== */

esp_err_t nimble_init(const char *device_name, const nimble_callbacks_t *cbs);
esp_err_t nimble_deinit(void);

/* ==================== Peripheral API ==================== */

void  nimble_start_advertising(void);
bool  nimble_is_connected(void);
int   nimble_send_notify(const uint8_t *data, uint16_t len);

/* ==================== Central API (新增) ==================== */

/**
 * @brief 启动 BLE 扫描，自动连接名称以 target_prefix 开头的设备
 * @param target_prefix  目标名称前缀（如 "Xbox"），NULL 则只扫描不自动连接
 * @param duration_sec   扫描持续时间（秒），0 = 持续扫描
 */
esp_err_t nimble_start_scan(const char *target_prefix, uint32_t duration_sec);

/** @brief 停止扫描 */
void nimble_stop_scan(void);

/** @brief 是否正在扫描 */
bool nimble_is_scanning(void);

/** @brief Central 连接是否已建立 */
bool nimble_central_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* NIMBLE_H */