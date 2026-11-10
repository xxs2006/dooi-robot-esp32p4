/**
 * @file remote_protocol.c
 * @brief 统一远程控制协议 — 指令解析与硬件调度
 */

#include "remote_protocol.h"

#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* ---------- 硬件层头文件 ---------- */
#include "app_periphral.h"
#include "app_ui.h"
#include "app_ui_logic.h"
#include "eyes_control.h"
#include "ir.h"
#include "app_imu.h"
#include "touch_button.h"       /* ← 新增 */

#define TAG "REMOTE_PROTO"

/* ================================================================
 *  辅助: 填充控制指令应答
 * ================================================================ */
static inline void fill_response(remote_response_t *rsp,
                                 uint8_t cmd_id,
                                 remote_rsp_code_t code)
{
    if (!rsp) return;
    rsp->pkt.rsp.marker = REMOTE_RSP_MARKER;
    rsp->pkt.rsp.cmd_id = cmd_id;
    rsp->pkt.rsp.status = (uint8_t)code;
    rsp->len = sizeof(remote_pkt_response_t);
}

/* ================================================================
 *  各指令处理函数
 * ================================================================ */

static remote_rsp_code_t handle_heartbeat(void)
{
    return REMOTE_RSP_OK;
}

static remote_rsp_code_t handle_joystick(const uint8_t *data, uint16_t len)
{
    if (len < sizeof(remote_pkt_joystick_t)) return REMOTE_RSP_ERR_BAD_LENGTH;

    const remote_pkt_joystick_t *pkt = (const remote_pkt_joystick_t *)data;

    float throttle = pkt->throttle;
    float steering = pkt->steering;

    if (throttle < -1.0f) throttle = -1.0f;
    if (throttle >  1.0f) throttle =  1.0f;
    if (steering < -1.0f) steering = -1.0f;
    if (steering >  1.0f) steering =  1.0f;

    if (fabsf(throttle) < 0.05f) throttle = 0.0f;
    if (fabsf(steering) < 0.05f) steering = 0.0f;

    app_motor_set_remote(throttle, steering);
    return REMOTE_RSP_OK;
}

static remote_rsp_code_t handle_motor_auto(const uint8_t *data, uint16_t len)
{
    if (len < sizeof(remote_pkt_motor_auto_t)) return REMOTE_RSP_ERR_BAD_LENGTH;

    const remote_pkt_motor_auto_t *pkt = (const remote_pkt_motor_auto_t *)data;
    esp_err_t ret = app_motor_move_auto(pkt->speed_l, pkt->speed_r, pkt->duration_ms);
    return (ret == ESP_OK) ? REMOTE_RSP_OK : REMOTE_RSP_ERR_BUSY;
}

static remote_rsp_code_t handle_led_rgb(const uint8_t *data, uint16_t len)
{
    if (len < sizeof(remote_pkt_led_rgb_t)) return REMOTE_RSP_ERR_BAD_LENGTH;

    const remote_pkt_led_rgb_t *pkt = (const remote_pkt_led_rgb_t *)data;
    app_led_set_rgb(pkt->left_color, pkt->right_color);
    return REMOTE_RSP_OK;
}

static remote_rsp_code_t handle_led_effect(const uint8_t *data, uint16_t len)
{
    if (len < sizeof(remote_pkt_led_effect_t)) return REMOTE_RSP_ERR_BAD_LENGTH;

    remote_pkt_led_effect_t *pkt = (remote_pkt_led_effect_t *)data;
    bool sym = (pkt->symmetric != 0);

    pkt->effect += 2;

    switch (pkt->effect) {
    case LED_EFFECT_BREATHING:
        app_led_set_breathing(pkt->color, pkt->period_ms, sym);
        break;
    case LED_EFFECT_RAINBOW:
        app_led_set_rainbow(pkt->period_ms, sym);
        break;
    case LED_EFFECT_BLINK:
        app_led_set_blink(pkt->color, pkt->period_ms, sym);
        break;
    default:
        return REMOTE_RSP_ERR_BAD_PARAM;
    }
    return REMOTE_RSP_OK;
}

static remote_rsp_code_t handle_led_off(void)
{
    app_led_off();
    return REMOTE_RSP_OK;
}

static remote_rsp_code_t handle_servo(const uint8_t *data, uint16_t len)
{
    if (len < sizeof(remote_pkt_servo_t)) return REMOTE_RSP_ERR_BAD_LENGTH;

    const remote_pkt_servo_t *pkt = (const remote_pkt_servo_t *)data;

    if (pkt->target > SERVO_TARGET_BOTH) return REMOTE_RSP_ERR_BAD_PARAM;
    if (pkt->angle < 0.0f || pkt->angle > 180.0f) return REMOTE_RSP_ERR_BAD_PARAM;

    app_servo_set_target_angle((servo_target_t)pkt->target, pkt->angle, pkt->duration_ms);
    return REMOTE_RSP_OK;
}

static remote_rsp_code_t handle_screen(const uint8_t *data, uint16_t len)
{
    if (len < sizeof(remote_pkt_screen_t)) return REMOTE_RSP_ERR_BAD_LENGTH;

    const remote_pkt_screen_t *pkt = (const remote_pkt_screen_t *)data;
    if (pkt->screen_type >= SCREEN_COUNT) return REMOTE_RSP_ERR_BAD_PARAM;

    app_ui_logic_request_screen((screen_type_t)pkt->screen_type);
    return REMOTE_RSP_OK;
}

static remote_rsp_code_t handle_eye_emotion(const uint8_t *data, uint16_t len)
{
    if (len < sizeof(remote_pkt_eye_emotion_t)) return REMOTE_RSP_ERR_BAD_LENGTH;

    const remote_pkt_eye_emotion_t *pkt = (const remote_pkt_eye_emotion_t *)data;
    if (pkt->emotion >= EYES_EMOTION_COUNT) return REMOTE_RSP_ERR_BAD_PARAM;

    eye_controller_apply_emotion((EYES_EMOTION)pkt->emotion);
    return REMOTE_RSP_OK;
}

static remote_rsp_code_t handle_eye_iris(const uint8_t *data, uint16_t len)
{
    if (len < sizeof(remote_pkt_eye_iris_t)) return REMOTE_RSP_ERR_BAD_LENGTH;

    const remote_pkt_eye_iris_t *pkt = (const remote_pkt_eye_iris_t *)data;
    if (pkt->iris_type >= EYES_IRIS_COUNT) return REMOTE_RSP_ERR_BAD_PARAM;

    eye_controller_apply_iris_texture((EYES_IRIS_DEFINE)pkt->iris_type);
    eye_controller_apply_emotion((EYES_EMOTION)(pkt->iris_type + 1));
    return REMOTE_RSP_OK;
}

/* ================================================================
 *  查询指令: IR 红外触发计数
 * ================================================================ */
static remote_rsp_code_t handle_ir_get(remote_response_t *rsp)
{
    remote_pkt_ir_data_t *ir = &rsp->pkt.ir;
    ir->marker = REMOTE_DATA_MARKER;
    ir->cmd_id = REMOTE_CMD_IR_GET;

    for (int i = 0; i < IR_SENSOR_NUM; i++) {
        ir_trigger_count_get((ir_sensor_id_t)i, &ir->counts[i]);
    }

    rsp->len = sizeof(remote_pkt_ir_data_t);
    return REMOTE_RSP_OK;
}

/* ================================================================
 *  查询指令: IMU 数据
 * ================================================================ */
static remote_rsp_code_t handle_imu_get(remote_response_t *rsp)
{
    app_imu_data_t imu_data;
    app_imu_get_data(&imu_data);

    remote_pkt_imu_data_t *imu = &rsp->pkt.imu;
    imu->marker = REMOTE_DATA_MARKER;
    imu->cmd_id = REMOTE_CMD_IMU_GET;
    imu->ax = imu_data.ax;
    imu->ay = imu_data.ay;
    imu->az = imu_data.az;
    imu->gx = imu_data.gx;
    imu->gy = imu_data.gy;
    imu->gz = imu_data.gz;

    rsp->len = sizeof(remote_pkt_imu_data_t);
    return REMOTE_RSP_OK;
}

/* ================================================================
 *  查询指令: 触摸计数
 *  应答格式: [0xFE][0x0D][click_L u32][click_R u32]
 *            [dbl_L u32][dbl_R u32][long_L u32][long_R u32]
 *            [total_L u32][total_R u32]
 * ================================================================ */
static remote_rsp_code_t handle_touch_get(remote_response_t *rsp)
{
    touch_btn_counts_t counts;
    esp_err_t ret = touch_button_get_counts(&counts);
    if (ret != ESP_OK) return REMOTE_RSP_ERR_BUSY;

    remote_pkt_touch_data_t *td = &rsp->pkt.touch;
    td->marker = REMOTE_DATA_MARKER;
    td->cmd_id = REMOTE_CMD_TOUCH_GET;

    for (int i = 0; i < 2; i++) {
        td->click[i]        = counts.click[i];
        td->double_click[i] = counts.double_click[i];
        td->long_press[i]   = counts.long_press[i];
        td->total[i]        = counts.total[i];
    }

    rsp->len = sizeof(remote_pkt_touch_data_t);
    return REMOTE_RSP_OK;
}

/* ================================================================
 *  控制指令: 触摸计数清零
 * ================================================================ */
static remote_rsp_code_t handle_touch_reset(void)
{
    touch_button_reset_counts();
    return REMOTE_RSP_OK;
}

/* ================================================================
 *  指令分发表
 * ================================================================ */
typedef remote_rsp_code_t (*cmd_handler_fn)(const uint8_t *data, uint16_t len);

static remote_rsp_code_t wrap_heartbeat(const uint8_t *d, uint16_t l) {
    (void)d; (void)l; return handle_heartbeat();
}
static remote_rsp_code_t wrap_led_off(const uint8_t *d, uint16_t l) {
    (void)d; (void)l; return handle_led_off();
}
static remote_rsp_code_t wrap_touch_reset(const uint8_t *d, uint16_t l) {
    (void)d; (void)l; return handle_touch_reset();
}

static const cmd_handler_fn s_handlers[REMOTE_CMD_MAX] = {
    [REMOTE_CMD_HEARTBEAT]   = wrap_heartbeat,
    [REMOTE_CMD_JOYSTICK]    = handle_joystick,
    [REMOTE_CMD_MOTOR_AUTO]  = handle_motor_auto,
    [REMOTE_CMD_LED_RGB]     = handle_led_rgb,
    [REMOTE_CMD_LED_EFFECT]  = handle_led_effect,
    [REMOTE_CMD_LED_OFF]     = wrap_led_off,
    [REMOTE_CMD_SERVO]       = handle_servo,
    [REMOTE_CMD_SCREEN]      = handle_screen,
    [REMOTE_CMD_EYE_EMOTION] = handle_eye_emotion,
    [REMOTE_CMD_EYE_IRIS]    = handle_eye_iris,
    [REMOTE_CMD_IR_GET]      = NULL,          // 查询指令，特殊处理
    [REMOTE_CMD_IMU_GET]     = NULL,          // 查询指令，特殊处理
    [REMOTE_CMD_TOUCH_GET]   = NULL,          // 查询指令，特殊处理
    [REMOTE_CMD_TOUCH_RESET] = wrap_touch_reset,  // 控制指令
};

/* ================================================================
 *  公开 API
 * ================================================================ */
remote_rsp_code_t remote_protocol_handle(const uint8_t *data, uint16_t len,
                                         remote_response_t *rsp)
{
    if (!data || len < 1) {
        fill_response(rsp, 0, REMOTE_RSP_ERR_BAD_LENGTH);
        return REMOTE_RSP_ERR_BAD_LENGTH;
    }

    uint8_t cmd_id = data[0];
    remote_rsp_code_t code;

    /* 查询指令: 直接填充数据应答包 */
    if (cmd_id == REMOTE_CMD_IR_GET) {
        if (rsp) {
            code = handle_ir_get(rsp);
        } else {
            code = REMOTE_RSP_OK;
        }
        if (code != REMOTE_RSP_OK) {
            ESP_LOGW(TAG, "IR_GET failed: %d", code);
            fill_response(rsp, cmd_id, code);
        }
        return code;
    }

    if (cmd_id == REMOTE_CMD_IMU_GET) {
        if (rsp) {
            code = handle_imu_get(rsp);
        } else {
            code = REMOTE_RSP_OK;
        }
        if (code != REMOTE_RSP_OK) {
            ESP_LOGW(TAG, "IMU_GET failed: %d", code);
            fill_response(rsp, cmd_id, code);
        }
        return code;
    }

    if (cmd_id == REMOTE_CMD_TOUCH_GET) {
        if (rsp) {
            code = handle_touch_get(rsp);
        } else {
            code = REMOTE_RSP_OK;
        }
        if (code != REMOTE_RSP_OK) {
            ESP_LOGW(TAG, "TOUCH_GET failed: %d", code);
            fill_response(rsp, cmd_id, code);
        }
        return code;
    }

    /* 控制指令: 统一分发 */
    if (cmd_id >= REMOTE_CMD_MAX || s_handlers[cmd_id] == NULL) {
        ESP_LOGW(TAG, "Unknown cmd: 0x%02X", cmd_id);
        code = REMOTE_RSP_ERR_UNKNOWN_CMD;
    } else {
        code = s_handlers[cmd_id](data, len);
        if (code != REMOTE_RSP_OK) {
            ESP_LOGW(TAG, "Cmd 0x%02X failed: %d", cmd_id, code);
        }
    }

    fill_response(rsp, cmd_id, code);
    return code;
}