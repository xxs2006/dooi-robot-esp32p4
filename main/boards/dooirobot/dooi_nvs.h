#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  统一参数存储中心（Dooi Robot）
 *
 *  设计约定
 *  1. 所有设备自有参数统一存放在 NVS 命名空间 "dooi" 下，本模块是唯一读写者；
 *     键名 / 类型 / 默认值 / 量程集中定义在 dooi_nvs.c 的参数表里。
 *  2. 首次烧录（空 NVS）时，init 会把每个参数的默认值写入并持久化，
 *     之后再读取永远得到确定值，不再出现“空板读不到默认值”的问题。
 *  3. 读接口全部命中 RAM 快照（一次加载、常驻），不再每次访问 NVS。
 *  4. 写接口统一做量程裁剪 + 去重（值未变化不写），降低 Flash 擦写；
 *     保存失败会如实返回错误（NVS 不可用时返回 ESP_ERR_NVS_INVALID_HANDLE），
 *     不会出现“返回成功但实际没存”。
 *  5. WiFi 凭证（ssid + password）统一委托 SsidManager 成对写入 "wifi"
 *     命名空间，保证重启后 WifiStation 能取到完整凭证自动连接。
 *  6. 内部状态有互斥锁保护，可从任意任务调用（httpd / UI / 触摸 / 电机）。
 *
 *  通用（原始缩放值）接口
 *  下面是“按参数 ID 直接读写”的统一接口，值为 **原始缩放整数**：
 *    - 一般整型参数：原值
 *    - trim_srv_*  ：角度 × 10（单位 0.1°）
 *    - mic_gain    ：dB   × 10（单位 0.1dB）
 *    - touch_sens  ：ratio × 1000
 *  日常使用请优先调用下方带具体类型/单位的语义化接口。
 * ================================================================ */

/** 所有持久化参数的唯一 ID（新增参数：在此追加 + 在参数表追加一行） */
typedef enum {
    DOOI_PARAM_EYE_IRIS = 0,   /* 眼睛虹膜索引 0..EYES_IRIS_COUNT-1，默认 7 */
    DOOI_PARAM_INV_MOT_L,      /* 左电机反向，默认 1 */
    DOOI_PARAM_INV_MOT_R,      /* 右电机反向，默认 1 */
    DOOI_PARAM_INV_SRV_L,      /* 左舵机反向，默认 0 */
    DOOI_PARAM_INV_SRV_R,      /* 右舵机反向，默认 0 */
    DOOI_PARAM_TRIM_SRV_L,     /* 左舵机微调 (×10, 0.1°)，默认 0 */
    DOOI_PARAM_TRIM_SRV_R,     /* 右舵机微调 (×10, 0.1°)，默认 0 */
    DOOI_PARAM_MIC_GAIN,       /* 麦克风增益 (×10, 0.1dB)，默认 270 = 27.0dB */
    DOOI_PARAM_IR_ENABLE,      /* 防跌落 IR 功能使能，默认 1 */
    DOOI_PARAM_TOUCH_SENS,     /* 触摸灵敏度 (×1000)，默认 7 = 0.007 */
    DOOI_PARAM_VOLUME,         /* 系统音量 0..100，默认 70 */
    DOOI_PARAM_COUNT
} dooi_param_t;

/* ================================================================
 *  模块初始化
 * ================================================================ */

/**
 * @brief 初始化统一参数存储中心。
 *
 *  - 加载 / 补全 "dooi" 命名空间参数（空板自动落盘默认值）
 *  - 确保 OTA URL 存在默认值
 *  - 打印完整参数快照（首行 ready(NVS-OK|RAM-ONLY) 可用于确认是否落盘成功）
 *
 * 只负责存储，不触发任何硬件副作用（音量等参数的应用由各子系统初始化时读取）。
 * 应在 nvs_flash_init() 之后、其它子系统初始化之前尽早调用。
 * 读取接口同样支持惰性初始化，未显式调用也不会读到脏数据。
 */
void dooi_nvs_init(void);

/**
 * @brief 读取指定参数的原始缩放值（永不失败；ID 越界返回 0）。
 */
int32_t dooi_nvs_param_get(dooi_param_t id);

/**
 * @brief 写入指定参数的原始缩放值（自动量程裁剪；值未变化则不写 NVS）。
 * @return ESP_OK；ESP_ERR_INVALID_ARG（ID 越界或参数表缺行）；
 *         ESP_ERR_NVS_INVALID_HANDLE（NVS 不可用：值仅在本次开机生效，未落盘）。
 */
esp_err_t dooi_nvs_param_set(dooi_param_t id, int32_t raw_value);

/**
 * @brief 将所有参数恢复为内置默认值并持久化（不重启、不触发副作用）。
 */
esp_err_t dooi_nvs_reset_defaults(void);

/* ================================================================
 *  Wi-Fi / OTA（命名空间 "wifi"）
 * ================================================================ */

/**
 * @brief 读取 OTA URL。
 * @param[out] url      输出缓冲区
 * @param[in]  max_len  缓冲区大小（含 '\0'）
 * @return ESP_OK / ESP_ERR_NVS_NOT_FOUND / 其他
 */
esp_err_t dooi_nvs_get_ota_url(char *url, size_t max_len);

/**
 * @brief 写入 OTA URL（与基础工程 Ota/Settings 共用同一键）。
 * @param url  以 '\0' 结尾的 URL 字符串
 */
esp_err_t dooi_nvs_set_ota_url(const char *url);

/**
 * @brief 读取保存的 Wi-Fi SSID（仅供展示，不含密码）。
 */
esp_err_t dooi_nvs_get_wifi_ssid(char *ssid, size_t max_len);

/**
 * @brief 保存 Wi-Fi SSID + 密码（经 SsidManager 成对写入），随后重启。
 *
 * 成功时内部调用 esp_restart()，永不返回；保存失败时直接返回错误且不重启。
 * 调用前请确保已完成 HTTP 响应（内部会延迟 1s 再重启）。
 */
esp_err_t dooi_nvs_set_wifi(const char *ssid, const char *password);

/* ================================================================
 *  设备参数（命名空间 "dooi"）
 * ================================================================ */

/**
 * @brief 读取眼睛虹膜索引。
 * @param[out] iris  0-based 索引值
 */
esp_err_t dooi_nvs_get_eye_iris(uint8_t *iris);

/**
 * @brief 写入眼睛虹膜索引，并立即应用到眼睛控制器。
 * @param iris  0-based 索引值（越界自动裁剪到合法范围）
 */
esp_err_t dooi_nvs_set_eye_iris(uint8_t iris);

/**
 * @brief 读取硬件反向配置（指针可为 NULL 表示不关心该字段）。
 */
esp_err_t dooi_nvs_get_hw_invert(bool *mot_l, bool *mot_r, bool *srv_l, bool *srv_r);

/**
 * @brief 写入硬件反向配置。
 */
esp_err_t dooi_nvs_set_hw_invert(bool mot_l, bool mot_r, bool srv_l, bool srv_r);

/**
 * @brief 读取舵机校准偏移量 (trim)
 * @param[out] trim_l  左舵机偏移角度（-30.0 ~ +30.0 度）
 * @param[out] trim_r  右舵机偏移角度（-30.0 ~ +30.0 度）
 */
esp_err_t dooi_nvs_get_servo_trim(float *trim_l, float *trim_r);

/**
 * @brief 写入舵机校准偏移量 (trim)
 * @param trim_l  左舵机偏移角度（-30.0 ~ +30.0 度，自动裁剪）
 * @param trim_r  右舵机偏移角度（-30.0 ~ +30.0 度，自动裁剪）
 */
esp_err_t dooi_nvs_set_servo_trim(float trim_l, float trim_r);

/**
 * @brief 读取麦克风增益 (0.0 ~ 34.0 dB)
 */
esp_err_t dooi_nvs_get_mic_gain(float *gain);

/**
 * @brief 写入麦克风增益 (0.0 ~ 34.0 dB，自动裁剪)
 */
esp_err_t dooi_nvs_set_mic_gain(float gain);

/* ----------------------------------------------------------------
 *  IR 功能使能（存储于 NVS_NS_DOOI）
 * ---------------------------------------------------------------- */

/**
 * @brief 读取 IR 功能使能状态。
 * @param[out] enable  true: 使能（默认）, false: 禁用
 */
esp_err_t dooi_nvs_get_ir_enable(bool *enable);

/**
 * @brief 写入 IR 功能使能状态。
 * @param enable  true: 使能, false: 禁用
 */
esp_err_t dooi_nvs_set_ir_enable(bool enable);

/* ----------------------------------------------------------------
 *  触摸按键灵敏度（thresh_ratio）
 * ---------------------------------------------------------------- */

/**
 * @brief 读取触摸按键灵敏度（thresh_ratio）
 * @param[out] ratio  默认值 0.007f
 * @note  值越小越灵敏，范围 0.001 ~ 0.050
 */
esp_err_t dooi_nvs_get_touch_sensitivity(float *ratio);

/**
 * @brief 写入触摸按键灵敏度
 * @param ratio  0.001 ~ 0.050，超出范围自动裁剪
 */
esp_err_t dooi_nvs_set_touch_sensitivity(float ratio);

/* ----------------------------------------------------------------
 *  系统音量
 * ---------------------------------------------------------------- */

/**
 * @brief 读取系统音量 (0 ~ 100)
 */
esp_err_t dooi_nvs_get_volume(int *volume);

/**
 * @brief 写入系统音量 (0 ~ 100，自动裁剪)
 * @note  仅持久化；应用到音频请另行调用 dooi_set_volume()。
 */
esp_err_t dooi_nvs_set_volume(int volume);

/* ================================================================
 *  出厂重置
 * ================================================================ */

/**
 * @brief 恢复出厂设置（清除所有持久化配置 + WiFi 凭证，写回默认值并重启）。
 */
void dooi_nvs_factory_reset(void);

#ifdef __cplusplus
}
#endif
