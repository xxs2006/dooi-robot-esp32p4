/**
 * @file dooi_nvs.c
 * @brief 统一参数存储中心（Dooi Robot 全系统配置的唯一入口）
 *
 * 重构要点（相对旧实现）：
 *  1. 单一命名空间 "dooi" 承载所有设备参数，键名/类型/默认值/量程集中在一张表，
 *     新增参数只需改表，不再散落魔法数字。
 *  2. 首次烧录（空 NVS）自动把每个参数写入并持久化默认值 → 每次启动结果一致；
 *     旧实现里“命名空间存在但键缺失”时默认值会退化成 0 的坑随之消失。
 *  3. 读取全部命中 RAM 快照，不再频繁 nvs_open/nvs_get。
 *  4. 写入统一限幅 + 去重（值未变不写），减少 Flash 擦写。
 *  5. 参数版本号 cfg_ver，为后续格式迁移留出入口。
 *  6. WiFi 凭证统一委托 SsidManager（C++ 侧 dooi_wifi_store_credentials 钩子），
 *     保证 ssid/password 成对写入，修复“只写 SSID → 重启后又进配网”。
 *  7. 所有 NVS 失败路径如实返回错误并打日志，绝不允许“假装保存成功”；
 *     首次烧录可用日志 `ready(NVS-OK|RAM-ONLY)` 一行判断默认值是否落盘。
 *  8. 内部状态（快照/句柄）由互斥锁保护：httpd、UI、触摸、电机等任务会并发访问。
 *
 *  注意：本模块只负责存储，不再反向调用音频/眼睛等业务层；参数应用（如恢复音量）
 *  由对应子系统在初始化时完成（见 dooirobot.cc GetAudioCodec()）。
 */

#include "dooi_nvs.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "eyes_control.h"   /* EYES_IRIS_COUNT / EYES_EMOTION_COUNT / eye_controller_apply_* */

#define TAG "DOOI_NVS"

/* 由板级 C++ 代码实现（见 dooirobot.cc），两者都以 SsidManager 为准：
 *  - store：把 ssid/password 成对写入 NVS
 *  - get  ：读取"最新一次配置"的 SSID（SsidManager 列表末位），避免展示到旧条目 */
esp_err_t dooi_wifi_store_credentials(const char *ssid, const char *password);
esp_err_t dooi_wifi_get_ssid(char *out, size_t max_len);
void      dooi_wifi_clear_credentials(void);

/* ================================================================
 *  命名空间 & 键
 *  - "dooi"：本模块自有参数（唯一写入者）
 *  - "wifi"：系统共享命名空间
 *      "ota_url"  —— Ota / Settings 也使用（Ota::GetCheckVersionUrl）
 *      "ssid"/"password(N)" —— WiFi 凭证，由 SsidManager 成对读写
 *                              （本模块只读 "ssid" 用于 /settings 展示）
 * ================================================================ */
#define NVS_NS_DOOI        "dooi"
#define NVS_NS_WIFI        "wifi"

#define NVS_KEY_OTA_URL    "ota_url"
#define NVS_KEY_WIFI_SSID  "ssid"
#define NVS_KEY_CFG_VER    "cfg_ver"

#define DEFAULT_OTA_URL    "https://api.tenclass.net/xiaozhi/ota/"
#define DOOI_MAX_OTA_URL_LEN  255      /* 写侧上限，读侧缓冲不应小于此值 */
#define DOOI_CFG_VERSION   1

/* nvs_open 失败后的重试次数（首次烧录时 NVS 可能正在格式化） */
#define CFG_OPEN_RETRY_MAX   3

/* ================================================================
 *  参数表 —— 全系统参数的唯一真相来源
 *  值均为“原始缩放整数”（见 dooi_nvs.h 说明）。
 * ================================================================ */
typedef enum { DOOI_T_U8, DOOI_T_I16, DOOI_T_U16 } dooi_type_t;

typedef struct {
    const char  *key;
    dooi_type_t  type;
    int32_t      def;
    int32_t      min;
    int32_t      max;
} dooi_desc_t;

static const dooi_desc_t k_desc[DOOI_PARAM_COUNT] = {
    [DOOI_PARAM_EYE_IRIS]   = { "eye_iris",   DOOI_T_U8,    7,      0,   EYES_IRIS_COUNT - 1 },
    [DOOI_PARAM_INV_MOT_L]  = { "inv_ml",     DOOI_T_U8,    1,      0,   1 },
    [DOOI_PARAM_INV_MOT_R]  = { "inv_mr",     DOOI_T_U8,    1,      0,   1 },
    [DOOI_PARAM_INV_SRV_L]  = { "inv_sl",     DOOI_T_U8,    0,      0,   1 },
    [DOOI_PARAM_INV_SRV_R]  = { "inv_sr",     DOOI_T_U8,    0,      0,   1 },
    [DOOI_PARAM_TRIM_SRV_L] = { "trim_sl",    DOOI_T_I16,   0,   -300, 300 },   /* 0.1°  */
    [DOOI_PARAM_TRIM_SRV_R] = { "trim_sr",    DOOI_T_I16,   0,   -300, 300 },   /* 0.1°  */
    [DOOI_PARAM_MIC_GAIN]   = { "mic_gain",   DOOI_T_I16, 270,      0, 340 },   /* 0.1dB，默认 27.0dB */
    [DOOI_PARAM_IR_ENABLE]  = { "ir_enable",  DOOI_T_U8,    1,      0,   1 },
    [DOOI_PARAM_TOUCH_SENS] = { "touch_sens", DOOI_T_U16,   7,      1,  50 },   /* ×1000，0.001~0.050 */
    [DOOI_PARAM_VOLUME]     = { "volume",     DOOI_T_U8,   70,      0, 100 },
};

/* 表行数必须与枚举一致：新增 DOOI_PARAM_* 却忘了加表行会在此编译报错 */
_Static_assert(sizeof(k_desc) / sizeof(k_desc[0]) == DOOI_PARAM_COUNT,
               "k_desc[] 行数与 dooi_param_t 不一致，请补全参数表");

/* ================================================================
 *  运行时状态
 * ================================================================ */
static int32_t           s_val[DOOI_PARAM_COUNT];   /* RAM 快照（受 s_lock 保护） */
static bool              s_loaded = false;           /* 已完成一次加载（失败超限后也为 true） */
static bool              s_persist_ok = false;       /* 句柄可用 → 可以持久化 */
static nvs_handle_t      s_handle = 0;              /* "dooi" 命名空间常开句柄 */
static int               s_open_fail = 0;            /* 连续 nvs_open 失败次数 */

/* 互斥锁：静态分配（无需堆内存），可在临界区内安全创建。
 * httpd / UI / 触摸 / 电机等多个任务会并发访问本模块状态。 */
static SemaphoreHandle_t s_lock = NULL;
static StaticSemaphore_t s_lock_buf;
static portMUX_TYPE      s_lock_init_mux = portMUX_INITIALIZER_UNLOCKED;

static void cfg_lock(void)
{
    if (s_lock == NULL) {
        portENTER_CRITICAL(&s_lock_init_mux);
        if (s_lock == NULL) {
            s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        }
        portEXIT_CRITICAL(&s_lock_init_mux);
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void cfg_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ----------------------------------------------------------------
 *  底层类型化读写
 * ---------------------------------------------------------------- */
static esp_err_t raw_read(nvs_handle_t h, const dooi_desc_t *d, int32_t *out)
{
    if (!d || !d->key || !out) return ESP_ERR_INVALID_ARG;
    switch (d->type) {
    case DOOI_T_U8: {
        uint8_t v = 0;
        esp_err_t r = nvs_get_u8(h, d->key, &v);
        if (r == ESP_OK) *out = v;
        return r;
    }
    case DOOI_T_I16: {
        int16_t v = 0;
        esp_err_t r = nvs_get_i16(h, d->key, &v);
        if (r == ESP_OK) *out = v;
        return r;
    }
    case DOOI_T_U16: {
        uint16_t v = 0;
        esp_err_t r = nvs_get_u16(h, d->key, &v);
        if (r == ESP_OK) *out = v;
        return r;
    }
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t raw_write(nvs_handle_t h, const dooi_desc_t *d, int32_t v)
{
    if (!d || !d->key) return ESP_ERR_INVALID_ARG;
    switch (d->type) {
    case DOOI_T_U8:  return nvs_set_u8 (h, d->key, (uint8_t)v);
    case DOOI_T_I16: return nvs_set_i16(h, d->key, (int16_t)v);
    case DOOI_T_U16: return nvs_set_u16(h, d->key, (uint16_t)v);
    }
    return ESP_ERR_INVALID_ARG;
}

/* ----------------------------------------------------------------
 *  加载 / 补全参数
 *  空 NVS 或越界数据一律写回默认值并持久化，保证后续读取确定。
 * ---------------------------------------------------------------- */
/* 调用者必须持有 s_lock */
static void cfg_load(void)
{
    if (s_handle == 0) {
        esp_err_t ret = nvs_open(NVS_NS_DOOI, NVS_READWRITE, &s_handle);
        if (ret != ESP_OK) {
            s_handle     = 0;
            s_persist_ok = false;
            for (int i = 0; i < DOOI_PARAM_COUNT; i++) s_val[i] = k_desc[i].def;

            s_open_fail++;
            ESP_LOGE(TAG, "nvs_open(%s) failed (%d/%d): %s — 参数仅存在于 RAM，"
                          "本次开机的修改都不会保存！",
                     NVS_NS_DOOI, s_open_fail, CFG_OPEN_RETRY_MAX, esp_err_to_name(ret));

            /* 失败次数未超限时允许下次调用重试；超限后不再反复尝试 */
            s_loaded = (s_open_fail >= CFG_OPEN_RETRY_MAX);
            return;
        }
        s_open_fail = 0;
    }
    s_persist_ok = true;

    bool dirty = false;
    for (int i = 0; i < DOOI_PARAM_COUNT; i++) {
        const dooi_desc_t *d = &k_desc[i];
        int32_t v = 0;
        if (raw_read(s_handle, d, &v) == ESP_OK && v >= d->min && v <= d->max) {
            s_val[i] = v;
        } else {
            /* 首次烧录（空板）或数据非法：写入默认值 */
            s_val[i] = d->def;
            esp_err_t r = raw_write(s_handle, d, d->def);
            if (r == ESP_OK) {
                dirty = true;
            } else {
                ESP_LOGE(TAG, "write default '%s'=%d failed: %s",
                         d->key, (int)d->def, esp_err_to_name(r));
            }
            ESP_LOGI(TAG, "'%s' -> default %d", d->key, (int)d->def);
        }
    }
    if (dirty) {
        esp_err_t r = nvs_commit(s_handle);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "commit defaults failed: %s — 下次开机会重新写入",
                     esp_err_to_name(r));
        }
    }

    /* 参数版本号（后续格式迁移入口：ver < DOOI_CFG_VERSION 时在此执行迁移） */
    uint8_t ver = 0;
    if (nvs_get_u8(s_handle, NVS_KEY_CFG_VER, &ver) != ESP_OK || ver < DOOI_CFG_VERSION) {
        esp_err_t r = nvs_set_u8(s_handle, NVS_KEY_CFG_VER, DOOI_CFG_VERSION);
        if (r == ESP_OK) r = nvs_commit(s_handle);
        if (r == ESP_OK) {
            ESP_LOGI(TAG, "cfg_ver -> %d", DOOI_CFG_VERSION);
        } else {
            ESP_LOGE(TAG, "write cfg_ver failed: %s", esp_err_to_name(r));
        }
    }

    s_loaded = true;
}

static void cfg_ensure_loaded(void)
{
    if (s_loaded) return;
    cfg_lock();
    if (!s_loaded) {
        cfg_load();
    }
    cfg_unlock();
}

/* ================================================================
 *  通用接口
 * ================================================================ */
int32_t dooi_nvs_param_get(dooi_param_t id)
{
    if (id < 0 || id >= DOOI_PARAM_COUNT) return 0;
    cfg_ensure_loaded();

    cfg_lock();
    int32_t v = s_val[id];
    cfg_unlock();
    return v;
}

esp_err_t dooi_nvs_param_set(dooi_param_t id, int32_t raw_value)
{
    if (id < 0 || id >= DOOI_PARAM_COUNT) return ESP_ERR_INVALID_ARG;

    const dooi_desc_t *d = &k_desc[id];
    if (!d->key) return ESP_ERR_INVALID_ARG;

    cfg_ensure_loaded();

    int32_t v = clamp_i32(raw_value, d->min, d->max);

    cfg_lock();

    if (v == s_val[id]) {          /* 值未变化：不写 NVS，避免无谓擦写 */
        cfg_unlock();
        return ESP_OK;
    }

    if (s_handle == 0 || !s_persist_ok) {
        /* 本次开机仍生效（写 RAM 快照），但绝不返回 ESP_OK —— 上层必须知道没落盘 */
        s_val[id] = v;
        cfg_unlock();
        ESP_LOGE(TAG, "'%s'=%d applied to RAM only, NOT persisted (NVS unavailable)",
                 d->key, (int)v);
        return ESP_ERR_NVS_INVALID_HANDLE;
    }

    esp_err_t ret = raw_write(s_handle, d, v);
    if (ret == ESP_OK) ret = nvs_commit(s_handle);

    if (ret == ESP_OK) {
        s_val[id] = v;
    } else {
        ESP_LOGE(TAG, "save '%s'=%d failed: %s", d->key, (int)v, esp_err_to_name(ret));
    }

    cfg_unlock();
    return ret;
}

esp_err_t dooi_nvs_reset_defaults(void)
{
    cfg_ensure_loaded();

    esp_err_t ret = ESP_OK;
    cfg_lock();

    if (s_handle == 0 || !s_persist_ok) {
        /* 退化为仅 RAM：快照仍刷成默认值，但如实上报错误 */
        for (int i = 0; i < DOOI_PARAM_COUNT; i++) s_val[i] = k_desc[i].def;
        cfg_unlock();
        ESP_LOGE(TAG, "reset_defaults: NVS not available");
        return ESP_ERR_NVS_INVALID_HANDLE;
    }

    for (int i = 0; i < DOOI_PARAM_COUNT; i++) {
        esp_err_t r = raw_write(s_handle, &k_desc[i], k_desc[i].def);
        if (r != ESP_OK && ret == ESP_OK) ret = r;
        s_val[i] = k_desc[i].def;
    }
    esp_err_t c = nvs_commit(s_handle);
    if (c != ESP_OK && ret == ESP_OK) ret = c;

    cfg_unlock();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "reset_defaults failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/* ================================================================
 *  模块初始化
 * ================================================================ */
void dooi_nvs_init(void)
{
    /* 1. 加载 / 补全 "dooi" 参数（空板会在此把默认值落盘） */
    cfg_ensure_loaded();

    /* 2. 确保 OTA URL 存在默认值（与基础工程共用 "wifi"/"ota_url"） */
    char url[128];
    if (dooi_nvs_get_ota_url(url, sizeof(url)) != ESP_OK) {
        if (dooi_nvs_set_ota_url(DEFAULT_OTA_URL) == ESP_OK) {
            ESP_LOGI(TAG, "OTA URL initialized to default");
        }
    }

    /* 3. 打印完整快照：现场可直接确认“默认值是否真的落盘”
     *    注意：音量等参数的应用由对应子系统负责（见 dooirobot.cc GetAudioCodec） */
    ESP_LOGI(TAG, "ready(%s): vol=%d iris=%d mic=%.1fdB ir=%d touch=%.3f "
                  "inv[m %d%d s %d%d] trim[%.1f/%.1f]",
             s_persist_ok ? "NVS-OK" : "RAM-ONLY",
             (int)dooi_nvs_param_get(DOOI_PARAM_VOLUME),
             (int)dooi_nvs_param_get(DOOI_PARAM_EYE_IRIS),
             dooi_nvs_param_get(DOOI_PARAM_MIC_GAIN) / 10.0f,
             (int)dooi_nvs_param_get(DOOI_PARAM_IR_ENABLE),
             dooi_nvs_param_get(DOOI_PARAM_TOUCH_SENS) / 1000.0f,
             (int)dooi_nvs_param_get(DOOI_PARAM_INV_MOT_L),
             (int)dooi_nvs_param_get(DOOI_PARAM_INV_MOT_R),
             (int)dooi_nvs_param_get(DOOI_PARAM_INV_SRV_L),
             (int)dooi_nvs_param_get(DOOI_PARAM_INV_SRV_R),
             dooi_nvs_param_get(DOOI_PARAM_TRIM_SRV_L) / 10.0f,
             dooi_nvs_param_get(DOOI_PARAM_TRIM_SRV_R) / 10.0f);
}

/* ================================================================
 *  Wi-Fi / OTA（命名空间 "wifi"）
 * ================================================================ */
esp_err_t dooi_nvs_get_ota_url(char *url, size_t max_len)
{
    if (!url || max_len == 0) return ESP_ERR_INVALID_ARG;
    url[0] = '\0';

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS_WIFI, NVS_READONLY, &h);
    if (ret != ESP_OK) return ret;

    size_t len = max_len;
    ret = nvs_get_str(h, NVS_KEY_OTA_URL, url, &len);
    nvs_close(h);
    if (ret != ESP_OK) {
        url[0] = '\0';
        return ret;
    }

    /* 空串视为未配置（不限定 scheme，兼容 listenai: 等自定义地址） */
    if (url[0] == '\0') {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t dooi_nvs_set_ota_url(const char *url)
{
    if (!url || url[0] == '\0') return ESP_ERR_INVALID_ARG;

    /* 过长的 URL 会在读侧因缓冲不足返回 INVALID_LENGTH，进而被默认值覆盖，直接拒绝 */
    size_t url_len = strlen(url);
    if (url_len > DOOI_MAX_OTA_URL_LEN) {
        ESP_LOGE(TAG, "set_ota_url: too long (%u > %d)",
                 (unsigned)url_len, DOOI_MAX_OTA_URL_LEN);
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS_WIFI, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", NVS_NS_WIFI, esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_str(h, NVS_KEY_OTA_URL, url);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set_ota_url failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t dooi_nvs_get_wifi_ssid(char *ssid, size_t max_len)
{
    if (!ssid || max_len == 0) return ESP_ERR_INVALID_ARG;
    ssid[0] = '\0';

    /* 优先问 SsidManager（它掌握"最新一次配置"的 SSID 及其配对密码），
     * 失败再回退到直接读 NVS 的 "ssid" 键 */
    if (dooi_wifi_get_ssid(ssid, max_len) == ESP_OK) {
        return ESP_OK;
    }
    ssid[0] = '\0';

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS_WIFI, NVS_READONLY, &h);
    if (ret != ESP_OK) return ret;

    size_t len = max_len;
    ret = nvs_get_str(h, NVS_KEY_WIFI_SSID, ssid, &len);
    nvs_close(h);

    if (ret != ESP_OK) {
        ssid[0] = '\0';
        return ret;
    }
    if (ssid[0] == '\0') {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t dooi_nvs_set_wifi(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

    /* 凭证统一交给 SsidManager：它把 ssid/password 成对写入 "wifi" 命名空间，
     * 重启后 WifiStation 才能从 SsidManager 取到完整凭证并自动连接。
     * （旧实现只写 "ssid" 不写 "password"，导致重启后凭证列表为空、又进配网） */
    esp_err_t ret = dooi_wifi_store_credentials(ssid, password ? password : "");
    if (ret != ESP_OK) {
        /* 保存失败就不要重启，否则设备重启后仍回到配网页，且用户无从得知原因 */
        ESP_LOGE(TAG, "store Wi-Fi credentials failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Wi-Fi config updated, ssid=%s — restarting...", ssid);

    /* 延迟重启，让调用方有时间发送 HTTP 响应 */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK; /* unreachable */
}

/* ================================================================
 *  设备参数（命名空间 "dooi"）
 * ================================================================ */
esp_err_t dooi_nvs_get_eye_iris(uint8_t *iris)
{
    if (!iris) return ESP_ERR_INVALID_ARG;
    *iris = (uint8_t)dooi_nvs_param_get(DOOI_PARAM_EYE_IRIS);
    return ESP_OK;
}

esp_err_t dooi_nvs_set_eye_iris(uint8_t iris)
{
    esp_err_t ret = dooi_nvs_param_set(DOOI_PARAM_EYE_IRIS, (int32_t)iris);

    /* 应用裁剪后的合法值，避免越界索引 */
    uint8_t v = (uint8_t)dooi_nvs_param_get(DOOI_PARAM_EYE_IRIS);
    eye_controller_apply_iris_texture((EYES_IRIS_DEFINE)v);

    /* 虹膜与情绪表一一对应（EYES_EMOTION = iris + 1）：任一枚举顺序变化时必须
     * 同步修改两处，这里做越界保护，避免静默写坏表情 */
    if ((int)v + 1 < (int)EYES_EMOTION_COUNT) {
        eye_controller_apply_emotion((EYES_EMOTION)(v + 1));
    }
    ESP_LOGI(TAG, "Eye iris set: %d", v);
    return ret;
}

esp_err_t dooi_nvs_get_hw_invert(bool *mot_l, bool *mot_r, bool *srv_l, bool *srv_r)
{
    if (mot_l) *mot_l = dooi_nvs_param_get(DOOI_PARAM_INV_MOT_L) != 0;
    if (mot_r) *mot_r = dooi_nvs_param_get(DOOI_PARAM_INV_MOT_R) != 0;
    if (srv_l) *srv_l = dooi_nvs_param_get(DOOI_PARAM_INV_SRV_L) != 0;
    if (srv_r) *srv_r = dooi_nvs_param_get(DOOI_PARAM_INV_SRV_R) != 0;
    return ESP_OK;
}

esp_err_t dooi_nvs_set_hw_invert(bool mot_l, bool mot_r, bool srv_l, bool srv_r)
{
    esp_err_t ret = ESP_OK;
    esp_err_t r;
    r = dooi_nvs_param_set(DOOI_PARAM_INV_MOT_L, mot_l ? 1 : 0); if (r != ESP_OK) ret = r;
    r = dooi_nvs_param_set(DOOI_PARAM_INV_MOT_R, mot_r ? 1 : 0); if (r != ESP_OK) ret = r;
    r = dooi_nvs_param_set(DOOI_PARAM_INV_SRV_L, srv_l ? 1 : 0); if (r != ESP_OK) ret = r;
    r = dooi_nvs_param_set(DOOI_PARAM_INV_SRV_R, srv_r ? 1 : 0); if (r != ESP_OK) ret = r;
    return ret;
}

/* ---- 舵机 trim（原始值单位 0.1°）---- */
esp_err_t dooi_nvs_get_servo_trim(float *trim_l, float *trim_r)
{
    if (trim_l) *trim_l = dooi_nvs_param_get(DOOI_PARAM_TRIM_SRV_L) / 10.0f;
    if (trim_r) *trim_r = dooi_nvs_param_get(DOOI_PARAM_TRIM_SRV_R) / 10.0f;
    return ESP_OK;
}

esp_err_t dooi_nvs_set_servo_trim(float trim_l, float trim_r)
{
    /* 限幅 ±30° 由参数表保证 */
    esp_err_t a = dooi_nvs_param_set(DOOI_PARAM_TRIM_SRV_L, (int32_t)lroundf(trim_l * 10.0f));
    esp_err_t b = dooi_nvs_param_set(DOOI_PARAM_TRIM_SRV_R, (int32_t)lroundf(trim_r * 10.0f));
    ESP_LOGI(TAG, "Servo trim saved: L=%.1f R=%.1f", trim_l, trim_r);
    return (a != ESP_OK) ? a : b;
}

/* ---- 麦克风增益（原始值单位 0.1dB）---- */
esp_err_t dooi_nvs_get_mic_gain(float *gain)
{
    if (!gain) return ESP_ERR_INVALID_ARG;
    *gain = dooi_nvs_param_get(DOOI_PARAM_MIC_GAIN) / 10.0f;
    return ESP_OK;
}

esp_err_t dooi_nvs_set_mic_gain(float gain)
{
    esp_err_t ret = dooi_nvs_param_set(DOOI_PARAM_MIC_GAIN, (int32_t)lroundf(gain * 10.0f));
    ESP_LOGI(TAG, "Mic gain saved: %.1f dB", gain);
    return ret;
}

/* ---- IR 功能使能 ---- */
esp_err_t dooi_nvs_get_ir_enable(bool *enable)
{
    if (!enable) return ESP_ERR_INVALID_ARG;
    *enable = dooi_nvs_param_get(DOOI_PARAM_IR_ENABLE) != 0;
    return ESP_OK;
}

esp_err_t dooi_nvs_set_ir_enable(bool enable)
{
    esp_err_t ret = dooi_nvs_param_set(DOOI_PARAM_IR_ENABLE, enable ? 1 : 0);
    ESP_LOGI(TAG, "IR enable saved: %s", enable ? "ON" : "OFF");
    return ret;
}

/* ---- 触摸灵敏度（原始值 = ratio × 1000）---- */
esp_err_t dooi_nvs_get_touch_sensitivity(float *ratio)
{
    if (!ratio) return ESP_ERR_INVALID_ARG;
    *ratio = dooi_nvs_param_get(DOOI_PARAM_TOUCH_SENS) / 1000.0f;
    return ESP_OK;
}

esp_err_t dooi_nvs_set_touch_sensitivity(float ratio)
{
    esp_err_t ret = dooi_nvs_param_set(DOOI_PARAM_TOUCH_SENS,
                                       (int32_t)lroundf(ratio * 1000.0f));
    ESP_LOGI(TAG, "Touch sensitivity saved: %.4f", ratio);
    return ret;
}

/* ---- 系统音量 ---- */
esp_err_t dooi_nvs_get_volume(int *volume)
{
    if (!volume) return ESP_ERR_INVALID_ARG;
    *volume = (int)dooi_nvs_param_get(DOOI_PARAM_VOLUME);
    return ESP_OK;
}

esp_err_t dooi_nvs_set_volume(int volume)
{
    /* 仅持久化；应用到音频由调用方负责（避免与 SetOutputVolume 递归） */
    return dooi_nvs_param_set(DOOI_PARAM_VOLUME, volume);
}

/* ================================================================
 *  出厂重置
 * ================================================================ */
/* 只删除本工程在 "wifi" 命名空间里的 OTA URL，避免影响共用该命名空间的其他组件
 * （WiFi 凭证由 dooi_wifi_clear_credentials 交给 SsidManager 统一清理） */
static void erase_ota_url_key(void)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS_WIFI, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", NVS_NS_WIFI, esp_err_to_name(ret));
        return;
    }

    nvs_erase_key(h, NVS_KEY_OTA_URL);

    ret = nvs_commit(h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "erase ota_url commit failed: %s", esp_err_to_name(ret));
    }
    nvs_close(h);
}

void dooi_nvs_factory_reset(void)
{
    ESP_LOGW(TAG, "=== FACTORY RESET INITIATED ===");

    nvs_handle_t h;

    /* 1. 释放常开句柄并复位状态（加锁，避免与其他任务交错） */
    cfg_lock();
    if (s_handle) {
        nvs_close(s_handle);
        s_handle = 0;
    }
    s_loaded     = false;
    s_persist_ok = false;
    s_open_fail  = 0;
    cfg_unlock();

    /* 2. 清空 "dooi" 命名空间 */
    if (nvs_open(NVS_NS_DOOI, NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t r = nvs_erase_all(h);
        if (r == ESP_OK) r = nvs_commit(h);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "erase 'dooi' failed: %s", esp_err_to_name(r));
        }
        nvs_close(h);
    } else {
        ESP_LOGE(TAG, "nvs_open(%s) failed during factory reset", NVS_NS_DOOI);
    }

    /* 3. 清空 WiFi 凭证（键名由 SsidManager 统一维护，同时清 RAM 列表）+ OTA URL */
    dooi_wifi_clear_credentials();
    erase_ota_url_key();

    /* 4. 清除 WiFi 协议栈底层保存的凭证（未初始化时返回 ESP_ERR_WIFI_NOT_INIT，可忽略） */
    esp_err_t wr = esp_wifi_restore();
    if (wr != ESP_OK && wr != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "esp_wifi_restore: %s", esp_err_to_name(wr));
    }

    /* 5. 重新落盘出厂默认值（含 cfg_ver） */
    cfg_ensure_loaded();

    /* 6. 写回默认 OTA URL */
    dooi_nvs_set_ota_url(DEFAULT_OTA_URL);

    ESP_LOGW(TAG, "NVS erased. Restarting in 1 second...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}
