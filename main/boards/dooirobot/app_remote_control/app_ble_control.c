#include "app_ble_control.h"
#include "nimble.h"
#include "remote_protocol.h"

#include <string.h>
#include "esp_log.h"

/* FreeRTOS */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "app_periphral.h"
#include "app_ui.h"
#include "eyes_control.h"
#include "app_ui_logic.h"
#include "xbox_controller.h"
#include "xbox_handle.h"

#define TAG              "APP_BLE_CTRL"
#define BLE_DEV_NAME     "DOOIROIBOT-BLE"
#define XBOX_NAME_PREFIX "Xbox"

/* ============================================================
 *  基础状态
 * ============================================================ */
static bool ble_control_enabled = false;

/* ============================================================
 *  扫描管理任务（Scan Manager Task）
 *
 *  生命周期：
 *    app_ble_trigger_xbox_scan() → scan_mgr_task_fn → vTaskDelete
 *
 *  事件位含义：
 *    BIT_CONNECTED    : Xbox 已连接
 *    BIT_DISCONNECTED : Xbox 已断开
 * ============================================================ */
#define SCAN_MGR_TIMEOUT_MS       15000          /* 15 s 无连接超时    */
#define SCAN_MGR_BIT_CONNECTED    (1UL << 0)     /* Xbox 连接成功      */
#define SCAN_MGR_BIT_DISCONNECTED (1UL << 1)     /* Xbox 断开连接      */

/* 使用 portMUX 保证 s_scan_running 标志的原子读写
 * portMUX_INITIALIZER_UNLOCKED 为静态初始化，无需动态创建 */
static portMUX_TYPE       s_scan_mux     = portMUX_INITIALIZER_UNLOCKED;
static volatile bool      s_scan_running = false;   /* 管理任务是否运行中 */
static EventGroupHandle_t s_scan_eg      = NULL;    /* 事件组（懒创建）   */
static TaskHandle_t       s_scan_task    = NULL;    /* 任务句柄           */

/* ---------- 内部：事件通知（由 Xbox 连接回调调用） ---------- */

static void scan_mgr_notify_connected(void)
{
    if (s_scan_eg != NULL) {
        xEventGroupSetBits(s_scan_eg, SCAN_MGR_BIT_CONNECTED);
    }
}

static void scan_mgr_notify_disconnected(void)
{
    if (s_scan_eg != NULL) {
        xEventGroupSetBits(s_scan_eg, SCAN_MGR_BIT_DISCONNECTED);
    }
}

/* ---------- 内部：安全停止 BLE 资源（防重复调用） ---------- */

static void scan_mgr_do_cleanup(void)
{
    if (app_ble_is_scanning()) {
        app_ble_stop_scan();
    }
    if (app_ble_control_is_enabled()) {
        app_ble_control_stop();
    }
}

/* ---------- 扫描管理任务主体 ---------- */

static void scan_mgr_task_fn(void *arg)
{
    ESP_LOGI(TAG, "[ScanMgr] 任务启动，等待 %d s 内设备连接...",
             SCAN_MGR_TIMEOUT_MS / 1000);

    /* ── 阶段 1：等待 CONNECTED（最多 15 s） ── */
    EventBits_t bits = xEventGroupWaitBits(
        s_scan_eg,
        SCAN_MGR_BIT_CONNECTED | SCAN_MGR_BIT_DISCONNECTED,
        pdFALSE,   /* 不自动清除 bit，阶段2仍需 DISCONNECTED */
        pdFALSE,   /* 任意 bit 满足即返回                    */
        pdMS_TO_TICKS(SCAN_MGR_TIMEOUT_MS)
    );

    if (!(bits & SCAN_MGR_BIT_CONNECTED)) {
        /* 超时或收到异常断开（未连接就触发断开）→ 直接关闭 */
        ESP_LOGW(TAG, "[ScanMgr] 15 s 内未建立连接 → 关闭蓝牙");
        goto cleanup;
    }

    /* ── 阶段 2：已连接，等待断开（无限期） ── */
    ESP_LOGI(TAG, "[ScanMgr] Xbox 已连接，监控断开事件...");

    xEventGroupWaitBits(
        s_scan_eg,
        SCAN_MGR_BIT_DISCONNECTED,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY
    );

    ESP_LOGW(TAG, "[ScanMgr] Xbox 断开连接 → 关闭蓝牙");

cleanup:
    /* 停止扫描 / 关闭 BLE */
    scan_mgr_do_cleanup();

    /* 重置运行标志（原子操作） */
    taskENTER_CRITICAL(&s_scan_mux);
    s_scan_running = false;
    s_scan_task    = NULL;
    taskEXIT_CRITICAL(&s_scan_mux);

    /* 清除事件位，供下次复用事件组 */
    xEventGroupClearBits(s_scan_eg,
                         SCAN_MGR_BIT_CONNECTED | SCAN_MGR_BIT_DISCONNECTED);

    ESP_LOGI(TAG, "[ScanMgr] 任务销毁");
    vTaskDelete(NULL);
}

/* ============================================================
 *  Peripheral 回调
 * ============================================================ */
static void parse_and_execute_command(uint8_t *data, uint16_t len)
{
    if (len < 1) return;

    remote_response_t rsp = {0};
    remote_protocol_handle(data, len, &rsp);
    nimble_send_notify((const uint8_t *)&rsp, sizeof(remote_response_t));
}

static void on_connection_state_changed(bool connected)
{
    ESP_LOGI(TAG, "Peripheral %s", connected ? "connected" : "disconnected");
}

/* ============================================================
 *  Central 回调
 * ============================================================ */
static void on_scan_result(const char *name, const uint8_t *addr,
                            uint8_t addr_type, int8_t rssi)
{
    ESP_LOGI(TAG, "Scan: %-20s  RSSI=%d  addr=%02x:%02x:%02x:%02x:%02x:%02x",
             name, rssi,
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

static void on_xbox_connection_changed(bool connected)
{
    if (connected) {
        ESP_LOGI(TAG, ">>> Xbox controller CONNECTED <<<");
        /* 通知扫描管理任务：连接成功 */
        scan_mgr_notify_connected();
    } else {
        ESP_LOGW(TAG, ">>> Xbox controller DISCONNECTED <<<");
        /* 通知扫描管理任务：断开连接 → 触发关闭流程 */
        scan_mgr_notify_disconnected();
    }
}

static void on_xbox_data_received(uint8_t *data, uint16_t len)
{
    xbox_controller_on_data(data, len);

    xbox_controller_state_t *xbox = xbox_controller_get_state();
    xbox_handle_process(xbox);
}

/* ============================================================
 *  公开 API
 * ============================================================ */
esp_err_t app_ble_control_start(void)
{
    /* 防重复开启 */
    if (ble_control_enabled) {
        ESP_LOGW(TAG, "BLE already enabled, skip re-init");
        return ESP_OK;
    }

    ble_control_enabled = true;

    app_event_t evt = {.type = APP_EVT_BLUETOOTH_OPEN};
    app_ui_logic_post(&evt, false);

    static const nimble_callbacks_t cbs = {
        .on_data_recv      = parse_and_execute_command,
        .on_conn_state     = on_connection_state_changed,
        .on_scan_result    = on_scan_result,
        .on_central_conn   = on_xbox_connection_changed,
        .on_central_notify = on_xbox_data_received,
    };

    esp_err_t ret = nimble_init(BLE_DEV_NAME, &cbs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_init failed: %d", ret);
        ble_control_enabled = false;   /* 回滚标志 */
        return ret;
    }

    ESP_LOGI(TAG, "App BLE control initialized");
    return ESP_OK;
}

void app_ble_start_advertising(void)
{
    nimble_start_advertising();
}

bool app_ble_is_connected(void)
{
    return nimble_is_connected();
}

void app_ble_send_data(const uint8_t *data, uint16_t len)
{
    nimble_send_notify(data, len);
}

void app_ble_control_stop(void)
{
    /* 防重复关闭 */
    if (!ble_control_enabled) {
        ESP_LOGW(TAG, "BLE already disabled, skip stop");
        return;
    }

    ble_control_enabled = false;

    app_event_t evt = {.type = APP_EVT_BLUETOOTH_CLOSE};
    app_ui_logic_post(&evt, false);

    nimble_deinit();
    ESP_LOGI(TAG, "App BLE control stopped");
}

bool app_ble_control_is_enabled(void)
{
    return ble_control_enabled;
}

/* ==================== Central 扫描 API ==================== */

esp_err_t app_ble_scan_xbox(void)
{
    if (!ble_control_enabled) {
        ESP_LOGE(TAG, "BLE not enabled, call app_ble_control_start() first");
        return ESP_ERR_INVALID_STATE;
    }

    /* 防重复开启扫描 */
    if (nimble_is_scanning()) {
        ESP_LOGW(TAG, "Already scanning, skip");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting scan for Xbox (prefix=\"%s\")...", XBOX_NAME_PREFIX);

    esp_err_t ret = nimble_start_scan(XBOX_NAME_PREFIX, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_start_scan failed: %d", ret);
    }
    return ret;
}

void app_ble_stop_scan(void)
{
    nimble_stop_scan();
    ESP_LOGI(TAG, "Xbox scan stopped");
}

bool app_ble_is_scanning(void)
{
    return nimble_is_scanning();
}

bool app_ble_xbox_is_connected(void)
{
    return nimble_central_is_connected();
}

/* ==================== 扫描管理任务 API ==================== */

void app_ble_trigger_xbox_scan(void)
{
    /* ── 步骤 1：原子检查并设置运行标志（防重复触发） ── */
    taskENTER_CRITICAL(&s_scan_mux);
    if (s_scan_running) {
        taskEXIT_CRITICAL(&s_scan_mux);
        ESP_LOGW(TAG, "[ScanMgr] 已在运行，忽略重复触发");
        return;
    }
    s_scan_running = true;   /* 先占位，后续失败再回滚 */
    taskEXIT_CRITICAL(&s_scan_mux);

    /* ── 步骤 2：懒创建事件组（整个生命周期只创建一次） ── */
    if (s_scan_eg == NULL) {
        s_scan_eg = xEventGroupCreate();
        if (s_scan_eg == NULL) {
            ESP_LOGE(TAG, "[ScanMgr] 事件组创建失败");
            goto rollback_flag;
        }
    }

    /* 清除上次遗留的事件位 */
    xEventGroupClearBits(s_scan_eg,
                         SCAN_MGR_BIT_CONNECTED | SCAN_MGR_BIT_DISCONNECTED);

    /* ── 步骤 3：开启 BLE（若未开启） ── */
    if (!app_ble_control_is_enabled()) {
        if (app_ble_control_start() != ESP_OK) {
            ESP_LOGE(TAG, "[ScanMgr] BLE 启动失败");
            goto rollback_flag;
        }
    }
    
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* ── 步骤 4：开始 Xbox 扫描 ── */
    if (app_ble_scan_xbox() != ESP_OK) {
        ESP_LOGE(TAG, "[ScanMgr] 扫描启动失败");
        app_ble_control_stop();   /* 回滚：关闭我们刚开的 BLE */
        goto rollback_flag;
    }

    /* ── 步骤 5：创建管理任务 ── */
    BaseType_t xRet = xTaskCreate(
        scan_mgr_task_fn,   /* 任务函数   */
        "ble_scan_mgr",     /* 任务名称   */
        4096,               /* 栈大小     */
        NULL,               /* 参数       */
        5,                  /* 优先级     */
        &s_scan_task        /* 任务句柄   */
    );

    if (xRet != pdPASS) {
        ESP_LOGE(TAG, "[ScanMgr] 任务创建失败");
        app_ble_stop_scan();
        app_ble_control_stop();
        goto rollback_flag;
    }

    ESP_LOGI(TAG, "[ScanMgr] 触发成功，管理任务已创建");
    return;

rollback_flag:
    taskENTER_CRITICAL(&s_scan_mux);
    s_scan_running = false;
    s_scan_task    = NULL;
    taskEXIT_CRITICAL(&s_scan_mux);
}