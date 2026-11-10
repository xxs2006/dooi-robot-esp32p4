#include "nimble.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"

/* --- NimBLE --- */
#include "esp_hosted.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define TAG "NIMBLE"

/* ============================================================
 *  内部类型
 * ============================================================ */

#define MAX_DISC_SVCS  16
#define MAX_DISC_CHRS  64

typedef struct {
    uint16_t start_handle;
    uint16_t end_handle;
} disc_svc_t;

typedef struct {
    uint16_t def_handle;
    uint16_t val_handle;
    uint8_t  properties;
    uint16_t svc_end_handle;   /* 所属 service 的 end_handle */
} disc_chr_t;

/* ============================================================
 *  静态变量 — Peripheral
 * ============================================================ */
static bool     s_is_connected  = false;
static uint16_t s_conn_handle   = 0;
static uint16_t s_notify_handle = 0;
static const char *s_device_name = "ESP32-BLE";

/* ============================================================
 *  静态变量 — Central (新增)
 * ============================================================ */
static bool     s_synced             = false;
static uint8_t  s_own_addr_type      = 0;

static bool     s_is_scanning        = false;
static bool     s_central_connecting = false;
static bool     s_central_connected  = false;
static uint16_t s_central_conn_handle = 0;
static char     s_target_prefix[32]  = {0};

/* 服务发现上下文 */
static disc_svc_t s_disc_svcs[MAX_DISC_SVCS];
static int        s_disc_svc_count = 0;
static int        s_disc_svc_idx   = 0;

static disc_chr_t s_disc_chrs[MAX_DISC_CHRS];
static int        s_disc_chr_count  = 0;
static int        s_subscribe_idx   = 0;

/* ============================================================
 *  回调
 * ============================================================ */
static nimble_callbacks_t s_callbacks = {0};

/* ============================================================
 *  UUID 定义 (Peripheral 服务)
 * ============================================================ */
static const ble_uuid128_t s_service_uuid =
    BLE_UUID128_INIT(0xfc, 0x35, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0xaa, 0xaa, 0x00, 0x00);

static const ble_uuid128_t s_char_uuid =
    BLE_UUID128_INIT(0xfc, 0x35, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0xbb, 0xbb, 0x00, 0x00);

/* ============================================================
 *  前向声明
 * ============================================================ */

/* 已有 */
static int  gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg);
static void nimble_advertise_internal(void);
static int  gap_event_handler(struct ble_gap_event *event, void *arg);
static void on_sync_cb(void);
static void ble_host_task(void *param);

/* Central 新增 */
static void start_service_discovery(uint16_t conn_handle);
static int  svc_disc_cb(uint16_t conn_handle,
                         const struct ble_gatt_error *error,
                         const struct ble_gatt_svc *service, void *arg);
static void discover_next_svc_chrs(uint16_t conn_handle);
static int  chr_disc_cb(uint16_t conn_handle,
                         const struct ble_gatt_error *error,
                         const struct ble_gatt_chr *chr, void *arg);
static void subscribe_next_chr(uint16_t conn_handle);
static int  dsc_disc_cb(uint16_t conn_handle,
                         const struct ble_gatt_error *error,
                         uint16_t chr_val_handle,
                         const struct ble_gatt_dsc *dsc, void *arg);
static int  subscribe_write_cb(uint16_t conn_handle,
                                const struct ble_gatt_error *error,
                                struct ble_gatt_attr *attr, void *arg);

/* ============================================================
 *  GATT 服务定义 (Peripheral)
 * ============================================================ */
static const struct ble_gatt_chr_def s_characteristics[] = {
    {
        .uuid       = &s_char_uuid.u,
        .access_cb  = gatt_access_cb,
        .flags      = BLE_GATT_CHR_F_WRITE |
                      BLE_GATT_CHR_F_WRITE_NO_RSP |
                      BLE_GATT_CHR_F_READ |
                      BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_notify_handle,
    },
    { 0 }
};

static const struct ble_gatt_svc_def s_gatt_services[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &s_service_uuid.u,
        .characteristics = s_characteristics,
    },
    { 0 }
};

/* ============================================================
 *  GATT 读写回调 (Peripheral)
 * ============================================================ */
static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const char *msg = "Hello from ESP32";
        os_mbuf_append(ctxt->om, msg, strlen(msg));
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0) return BLE_ATT_ERR_INSUFFICIENT_RES;
        if (len > 512) len = 512;

        uint8_t *data = malloc(len);
        if (!data) return BLE_ATT_ERR_INSUFFICIENT_RES;

        int rc = ble_hs_mbuf_to_flat(ctxt->om, data, len, NULL);
        if (rc == 0) {
            ESP_LOGI(TAG, "Peripheral RX %d bytes", len);
            if (s_callbacks.on_data_recv) {
                s_callbacks.on_data_recv(data, len);
            }
        }
        free(data);
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/* ============================================================
 *  GAP 事件处理 (Peripheral + Central 统一)
 * ============================================================ */
static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    int rc;

    switch (event->type) {

    /* ---------- 连接建立 ---------- */
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status == 0) {
            struct ble_gap_conn_desc desc;
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            if (rc != 0) {
                ESP_LOGE(TAG, "conn_find failed, rc=%d", rc);
                break;
            }

            if (desc.role == BLE_GAP_ROLE_MASTER) {
                /* === Central 角色连接成功 === */
                s_central_connecting = false;
                s_central_connected  = true;
                s_central_conn_handle = event->connect.conn_handle;

                // ESP_LOGI(TAG, "Central connected (handle=%d)", s_central_conn_handle);

                if (s_callbacks.on_central_conn) {
                    s_callbacks.on_central_conn(true);
                }

                /* 启动服务发现 */
                start_service_discovery(event->connect.conn_handle);

            } else {
                /* === Peripheral 角色连接成功 === */
                s_is_connected = true;
                s_conn_handle  = event->connect.conn_handle;

                // ESP_LOGI(TAG, "Peripheral connected (handle=%d, notify_h=%d)",
                //          s_conn_handle, s_notify_handle);

                if (s_callbacks.on_conn_state) {
                    s_callbacks.on_conn_state(true);
                }
            }
        } else {
            /* 连接失败 */
            ESP_LOGW(TAG, "Connection failed; status=%d", event->connect.status);

            if (s_central_connecting) {
                s_central_connecting = false;
                ESP_LOGI(TAG, "Central connect failed, restarting scan...");
                nimble_start_scan(s_target_prefix, 0);
            } else {
                nimble_advertise_internal();
            }
        }
        break;
    }

    /* ---------- 断开 ---------- */
    case BLE_GAP_EVENT_DISCONNECT: {
        uint16_t dh = event->disconnect.conn.conn_handle;

        if (s_central_connected && dh == s_central_conn_handle) {
            /* Central 断开 */
            s_central_connected   = false;
            s_central_conn_handle = 0;
            ESP_LOGI(TAG, "Central disconnected; reason=%d",
                     event->disconnect.reason);

            if (s_callbacks.on_central_conn) {
                s_callbacks.on_central_conn(false);
            }
            /* 不自动重连，由应用层决定 */

        } else {
            /* Peripheral 断开 */
            s_is_connected = false;
            s_conn_handle  = 0;
            ESP_LOGI(TAG, "Peripheral disconnected; reason=%d",
                     event->disconnect.reason);

            if (s_callbacks.on_conn_state) {
                s_callbacks.on_conn_state(false);
            }
            nimble_advertise_internal();
        }
        break;
    }

    /* ---------- 扫描到设备 (Central) ---------- */
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        rc = ble_hs_adv_parse_fields(&fields,
                                      event->disc.data,
                                      event->disc.length_data);
        if (rc != 0) break;

        char name[64] = {0};
        if (fields.name != NULL && fields.name_len > 0) {
            int clen = (fields.name_len < (int)sizeof(name) - 1)
                       ? fields.name_len : (int)sizeof(name) - 1;
            memcpy(name, fields.name, clen);
        }

        /* 上报给应用层 */
        if (name[0] && s_callbacks.on_scan_result) {
            s_callbacks.on_scan_result(name,
                                       event->disc.addr.val,
                                       event->disc.addr.type,
                                       event->disc.rssi);
        }

        /* 匹配目标前缀 → 自动连接 */
        if (s_target_prefix[0] &&
            strncmp(name, s_target_prefix, strlen(s_target_prefix)) == 0) {

            ESP_LOGI(TAG, "*** Target found: \"%s\" RSSI=%d, connecting ***",
                     name, event->disc.rssi);

            ble_gap_disc_cancel();
            s_is_scanning = false;
            s_central_connecting = true;

            rc = ble_gap_connect(s_own_addr_type,
                                 &event->disc.addr,
                                 30000,   /* 30 s 超时 */
                                 NULL,    /* 使用默认连接参数 */
                                 gap_event_handler, NULL);
            if (rc != 0 && rc != BLE_HS_EALREADY) {
                ESP_LOGE(TAG, "ble_gap_connect failed, rc=%d", rc);
                s_central_connecting = false;
                nimble_start_scan(s_target_prefix, 0);
            }
        }
        break;
    }

    /* ---------- 扫描完成 ---------- */
    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "Scan complete; reason=%d", event->disc_complete.reason);
        s_is_scanning = false;
        break;

    /* ---------- 广播完成 ---------- */
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "Adv complete; reason=%d", event->adv_complete.reason);
        if (!s_is_scanning && !s_central_connecting) {
            nimble_advertise_internal();
        }
        break;

    /* ---------- 收到 Notification/Indication (Central) ---------- */
    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        uint8_t *data = malloc(len);
        if (data) {
            ble_hs_mbuf_to_flat(event->notify_rx.om, data, len, NULL);

            // ESP_LOGI(TAG, "Notify RX: handle=%d len=%d ind=%d",
            //          event->notify_rx.attr_handle, len,
            //          event->notify_rx.indication);
            // ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, len, ESP_LOG_DEBUG);

            if (s_callbacks.on_central_notify) {
                s_callbacks.on_central_notify(data, len);
            }
            free(data);
        }
        break;
    }

    /* ---------- MTU 更新 ---------- */
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU update: conn=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        break;

    /* ---------- 加密变化 ---------- */
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "Encryption change: conn=%d status=%d",
                 event->enc_change.conn_handle, event->enc_change.status);
        break;

    /* ---------- 重复配对 ---------- */
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        ESP_LOGD(TAG, "GAP event %d", event->type);
        break;
    }

    return 0;
}

/* ============================================================
 *  广播 (Peripheral)
 * ============================================================ */
static void nimble_advertise_internal(void)
{
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields rsp_fields;
    struct ble_gap_adv_params adv_params;
    int rc;

    memset(&fields, 0, sizeof(fields));
    fields.flags                 = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128              = (ble_uuid128_t[]){ s_service_uuid };
    fields.num_uuids128          = 1;
    fields.uuids128_is_complete  = 1;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl            = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields rc=%d", rc); return; }

    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.name             = (uint8_t *)s_device_name;
    rsp_fields.name_len         = strlen(s_device_name);
    rsp_fields.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_rsp_set rc=%d", rc); return; }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    ESP_LOGI(TAG, "Starting advertising...");
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
    }
}

/* ============================================================
 *  服务发现链 (Central 新增)
 * ============================================================ */

/* --- 步骤 1: 发现所有 Service --- */
static void start_service_discovery(uint16_t conn_handle)
{
    ESP_LOGI(TAG, ">>> Starting service discovery...");
    s_disc_svc_count = 0;
    s_disc_chr_count = 0;
    s_disc_svc_idx   = 0;
    s_subscribe_idx  = 0;

    int rc = ble_gattc_disc_all_svcs(conn_handle, svc_disc_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "disc_all_svcs failed, rc=%d", rc);
    }
}

static int svc_disc_cb(uint16_t conn_handle,
                        const struct ble_gatt_error *error,
                        const struct ble_gatt_svc *service, void *arg)
{
    if (error->status == 0 && service != NULL) {
        if (s_disc_svc_count < MAX_DISC_SVCS) {
            s_disc_svcs[s_disc_svc_count].start_handle = service->start_handle;
            s_disc_svcs[s_disc_svc_count].end_handle   = service->end_handle;
            s_disc_svc_count++;

            char buf[BLE_UUID_STR_LEN];
            ble_uuid_to_str(&service->uuid.u, buf);
            ESP_LOGI(TAG, "  SVC: %s [%d-%d]",
                     buf, service->start_handle, service->end_handle);
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "Service discovery done (%d services)", s_disc_svc_count);
        s_disc_svc_idx = 0;
        discover_next_svc_chrs(conn_handle);
    } else {
        ESP_LOGE(TAG, "svc_disc error %d", error->status);
    }
    return 0;
}

/* --- 步骤 2: 逐个 Service 发现 Characteristic --- */
static void discover_next_svc_chrs(uint16_t conn_handle)
{
    if (s_disc_svc_idx >= s_disc_svc_count) {
        ESP_LOGI(TAG, "All chars discovered (%d total)", s_disc_chr_count);
        s_subscribe_idx = 0;
        subscribe_next_chr(conn_handle);
        return;
    }

    disc_svc_t *svc = &s_disc_svcs[s_disc_svc_idx];
    ESP_LOGI(TAG, "Discovering chars for SVC %d/%d [%d-%d]",
             s_disc_svc_idx + 1, s_disc_svc_count,
             svc->start_handle, svc->end_handle);

    int rc = ble_gattc_disc_all_chrs(conn_handle,
                                      svc->start_handle,
                                      svc->end_handle,
                                      chr_disc_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "disc_all_chrs failed, rc=%d", rc);
        s_disc_svc_idx++;
        discover_next_svc_chrs(conn_handle);
    }
}

static int chr_disc_cb(uint16_t conn_handle,
                        const struct ble_gatt_error *error,
                        const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0 && chr != NULL) {
        if (s_disc_chr_count < MAX_DISC_CHRS) {
            disc_chr_t *d = &s_disc_chrs[s_disc_chr_count];
            d->def_handle     = chr->def_handle;
            d->val_handle     = chr->val_handle;
            d->properties     = chr->properties;
            d->svc_end_handle = s_disc_svcs[s_disc_svc_idx].end_handle;
            s_disc_chr_count++;

            char buf[BLE_UUID_STR_LEN];
            ble_uuid_to_str(&chr->uuid.u, buf);
            ESP_LOGI(TAG, "    CHR: %s vh=%d props=0x%02x",
                     buf, chr->val_handle, chr->properties);
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        s_disc_svc_idx++;
        discover_next_svc_chrs(conn_handle);
    } else {
        ESP_LOGE(TAG, "chr_disc error %d", error->status);
        s_disc_svc_idx++;
        discover_next_svc_chrs(conn_handle);
    }
    return 0;
}

/* --- 步骤 3: 订阅所有支持 Notify/Indicate 的特征 --- */
static void subscribe_next_chr(uint16_t conn_handle)
{
    /* 跳到下一个支持 Notify 或 Indicate 的 Characteristic */
    while (s_subscribe_idx < s_disc_chr_count) {
        uint8_t p = s_disc_chrs[s_subscribe_idx].properties;
        if (p & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE)) {
            break;
        }
        s_subscribe_idx++;
    }

    if (s_subscribe_idx >= s_disc_chr_count) {
        ESP_LOGI(TAG, "=== Notification subscription complete ===");
        return;
    }

    disc_chr_t *cur = &s_disc_chrs[s_subscribe_idx];

    /* 计算描述符搜索的 end_handle */
    uint16_t end_handle = cur->svc_end_handle;
    if (s_subscribe_idx + 1 < s_disc_chr_count) {
        uint16_t next_def = s_disc_chrs[s_subscribe_idx + 1].def_handle - 1;
        if (next_def < end_handle) {
            end_handle = next_def;
        }
    }

    ESP_LOGI(TAG, "Discovering DSC for CHR vh=%d [%d-%d]",
             cur->val_handle, cur->val_handle, end_handle);

    int rc = ble_gattc_disc_all_dscs(conn_handle,
                                      cur->val_handle,
                                      end_handle,
                                      dsc_disc_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "disc_all_dscs failed, rc=%d", rc);
        s_subscribe_idx++;
        subscribe_next_chr(conn_handle);
    }
}

static int dsc_disc_cb(uint16_t conn_handle,
                        const struct ble_gatt_error *error,
                        uint16_t chr_val_handle,
                        const struct ble_gatt_dsc *dsc, void *arg)
{
    if (error->status == 0 && dsc != NULL) {
        /* 只关心 CCCD (0x2902) */
        if (ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(0x2902)) == 0) {
            /* 根据特征属性决定写入值 */
            uint8_t value[2] = {0x00, 0x00};
            disc_chr_t *cur  = &s_disc_chrs[s_subscribe_idx];
            if (cur->properties & BLE_GATT_CHR_PROP_NOTIFY)   value[0] |= 0x01;
            if (cur->properties & BLE_GATT_CHR_PROP_INDICATE) value[0] |= 0x02;

            ESP_LOGI(TAG, "  Writing CCCD handle=%d value=0x%02x%02x",
                     dsc->handle, value[1], value[0]);

            ble_gattc_write_flat(conn_handle, dsc->handle,
                                 value, sizeof(value),
                                 subscribe_write_cb, NULL);
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        s_subscribe_idx++;
        subscribe_next_chr(conn_handle);
    } else {
        ESP_LOGE(TAG, "dsc_disc error %d", error->status);
        s_subscribe_idx++;
        subscribe_next_chr(conn_handle);
    }
    return 0;
}

static int subscribe_write_cb(uint16_t conn_handle,
                                const struct ble_gatt_error *error,
                                struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == 0) {
        ESP_LOGI(TAG, "  CCCD write OK (subscribed)");
    } else {
        ESP_LOGW(TAG, "  CCCD write failed, status=%d", error->status);
    }
    return 0;
}

/* ============================================================
 *  扫描 API (Central 新增)
 * ============================================================ */
esp_err_t nimble_start_scan(const char *target_prefix, uint32_t duration_sec)
{
    if (!s_synced) {
        ESP_LOGE(TAG, "BLE stack not synced yet");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_is_scanning) {
        ESP_LOGW(TAG, "Already scanning");
        return ESP_OK;
    }
    if (s_central_connected) {
        ESP_LOGW(TAG, "Already connected as central");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_central_connecting) {
        ESP_LOGW(TAG, "Central connection in progress");
        return ESP_ERR_INVALID_STATE;
    }

    /* 保存目标前缀 */
    if (target_prefix && target_prefix[0]) {
        strncpy(s_target_prefix, target_prefix, sizeof(s_target_prefix) - 1);
        s_target_prefix[sizeof(s_target_prefix) - 1] = '\0';
    } else {
        s_target_prefix[0] = '\0';
    }

    /* 停止广播，避免冲突 */
    ble_gap_adv_stop();

    struct ble_gap_disc_params disc_params = {0};
    disc_params.filter_duplicates = 1;
    disc_params.passive           = 0;   /* 主动扫描，获取 Scan Response 中的设备名 */
    disc_params.itvl              = 0;   /* 默认 */
    disc_params.window            = 0;
    disc_params.filter_policy     = 0;
    disc_params.limited           = 0;

    int32_t dur_ms = (duration_sec > 0)
                     ? (int32_t)(duration_sec * 1000)
                     : BLE_HS_FOREVER;

    int rc = ble_gap_disc(s_own_addr_type, dur_ms,
                          &disc_params, gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed, rc=%d", rc);
        return ESP_FAIL;
    }

    s_is_scanning = true;
    ESP_LOGI(TAG, "Scanning started (prefix=\"%s\", dur=%lus)",
             s_target_prefix, (unsigned long)duration_sec);
    return ESP_OK;
}

void nimble_stop_scan(void)
{
    if (s_is_scanning) {
        ble_gap_disc_cancel();
        s_is_scanning = false;
        ESP_LOGI(TAG, "Scan stopped");
    }
}

bool nimble_is_scanning(void)
{
    return s_is_scanning;
}

bool nimble_central_is_connected(void)
{
    return s_central_connected;
}

/* ============================================================
 *  同步回调 & 主机任务
 * ============================================================ */
static void on_sync_cb(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    assert(rc == 0);

    s_synced = true;
    ESP_LOGI(TAG, "BLE stack synced (addr_type=%d)", s_own_addr_type);

    nimble_advertise_internal();
}

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ============================================================
 *  公开 API — 初始化
 * ============================================================ */
esp_err_t nimble_init(const char *device_name, const nimble_callbacks_t *cbs)
{
    esp_err_t ret;

    if (device_name) s_device_name = device_name;
    if (cbs)         s_callbacks   = *cbs;

    /* esp_hosted 连接 */
    ret = esp_hosted_connect_to_slave();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "hosted connect fail: %d", ret);
        return ret;
    }
    if (ESP_OK != esp_hosted_bt_controller_init()) {
        ESP_LOGE(TAG, "BT ctrl init fail");
        return ESP_FAIL;
    }
    if (ESP_OK != esp_hosted_bt_controller_enable()) {
        ESP_LOGE(TAG, "BT ctrl enable fail");
        return ESP_FAIL;
    }

    /* NimBLE 端口初始化 */
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init fail: %d", ret);
        return ret;
    }

    /* Host 回调 */
    ble_hs_cfg.sync_cb = on_sync_cb;

    /* 安全管理器（Just Works，无输入输出） */
    ble_hs_cfg.sm_io_cap         = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding        = 1;
    ble_hs_cfg.sm_mitm           = 0;
    ble_hs_cfg.sm_sc             = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;

    /* GAP / GATT 标准服务 */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    /* 自定义 Peripheral 服务 */
    int rc = ble_gatts_count_cfg(s_gatt_services);
    if (rc != 0) { ESP_LOGE(TAG, "svc count fail rc=%d", rc); return ESP_FAIL; }
    rc = ble_gatts_add_svcs(s_gatt_services);
    if (rc != 0) { ESP_LOGE(TAG, "svc add fail rc=%d", rc); return ESP_FAIL; }

    ble_svc_gap_device_name_set(s_device_name);

    /* 启动 Host 任务 */
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "NimBLE initialized [%s]", s_device_name);
    return ESP_OK;
}

/* ============================================================
 *  公开 API — Peripheral
 * ============================================================ */
void nimble_start_advertising(void)
{
    nimble_advertise_internal();
}

bool nimble_is_connected(void)
{
    return s_is_connected;
}

int nimble_send_notify(const uint8_t *data, uint16_t len)
{
    if (!s_is_connected) {
        ESP_LOGW(TAG, "Not connected (peripheral)");
        return -1;
    }
    if (s_notify_handle == 0) {
        ESP_LOGE(TAG, "Notify handle not init");
        return -2;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        ESP_LOGE(TAG, "mbuf alloc fail");
        return -3;
    }

    int rc = ble_gatts_notify_custom(s_conn_handle, s_notify_handle, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "notify fail rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "Sent %d bytes via notify", len);
    }
    return rc;
}

/* ============================================================
 *  公开 API — 反初始化
 * ============================================================ */
esp_err_t nimble_deinit(void)
{
    /* 停止扫描 */
    if (s_is_scanning) {
        ble_gap_disc_cancel();
        s_is_scanning = false;
    }

    /* 停止广播 */
    ble_gap_adv_stop();

    /* 断开 Central 连接 */
    if (s_central_connected) {
        ble_gap_terminate(s_central_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_central_connected   = false;
        s_central_conn_handle = 0;
    }

    /* 断开 Peripheral 连接 */
    if (s_is_connected) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_is_connected = false;
        s_conn_handle  = 0;
    }

    int rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_stop fail rc=%d", rc);
        return ESP_FAIL;
    }

    nimble_port_deinit();
    esp_hosted_bt_controller_disable();
    esp_hosted_bt_controller_deinit(true);

    s_synced             = false;
    s_central_connecting = false;
    s_target_prefix[0]   = '\0';

    ESP_LOGI(TAG, "NimBLE deinitialized");
    return ESP_OK;
}