/**
 * @file remote_protocol.h
 * @brief 统一远程控制协议 — 协议定义 + 指令分发接口
 */

#ifndef REMOTE_PROTOCOL_H
#define REMOTE_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  指令 ID
 * ================================================================ */
typedef enum {
    REMOTE_CMD_HEARTBEAT   = 0x00,
    REMOTE_CMD_JOYSTICK    = 0x01,
    REMOTE_CMD_MOTOR_AUTO  = 0x02,
    REMOTE_CMD_LED_RGB     = 0x04,
    REMOTE_CMD_LED_EFFECT  = 0x05,
    REMOTE_CMD_LED_OFF     = 0x06,
    REMOTE_CMD_SERVO       = 0x07,
    REMOTE_CMD_SCREEN      = 0x08,
    REMOTE_CMD_EYE_EMOTION = 0x09,
    REMOTE_CMD_EYE_IRIS    = 0x0A,
    REMOTE_CMD_IR_GET      = 0x0B,
    REMOTE_CMD_IMU_GET     = 0x0C,
    REMOTE_CMD_TOUCH_GET   = 0x0D,  /* ← 新增: 查询触摸计数 */
    REMOTE_CMD_TOUCH_RESET = 0x0E,  /* ← 新增: 清零触摸计数 */
    REMOTE_CMD_MAX
} remote_cmd_id_t;

/* ================================================================
 *  应答状态码
 * ================================================================ */
typedef enum {
    REMOTE_RSP_OK              = 0x00,
    REMOTE_RSP_ERR_UNKNOWN_CMD = 0x01,
    REMOTE_RSP_ERR_BAD_LENGTH  = 0x02,
    REMOTE_RSP_ERR_BAD_PARAM   = 0x03,
    REMOTE_RSP_ERR_BUSY        = 0x04,
} remote_rsp_code_t;

/* ================================================================
 *  数据包结构体 (1 字节对齐, Little-Endian)
 * ================================================================ */
#pragma pack(push, 1)

/* --- 通用头 --- */
typedef struct {
    uint8_t cmd_id;
} remote_pkt_header_t;

/* 0x01 摇杆 */
typedef struct {
    remote_pkt_header_t hdr;
    float throttle;
    float steering;
} remote_pkt_joystick_t;

/* 0x02 自动移动 */
typedef struct {
    remote_pkt_header_t hdr;
    int16_t  speed_l;
    int16_t  speed_r;
    uint16_t duration_ms;
} remote_pkt_motor_auto_t;

/* 0x04 LED RGB */
typedef struct {
    remote_pkt_header_t hdr;
    uint32_t left_color;
    uint32_t right_color;
} remote_pkt_led_rgb_t;

/* 0x05 LED 特效 */
typedef struct {
    remote_pkt_header_t hdr;
    uint8_t  effect;
    uint32_t color;
    uint16_t period_ms;
    uint8_t  symmetric;
} remote_pkt_led_effect_t;

/* 0x07 舵机 */
typedef struct {
    remote_pkt_header_t hdr;
    uint8_t  target;
    float    angle;
    uint32_t duration_ms;
} remote_pkt_servo_t;

/* 0x08 屏幕切换 */
typedef struct {
    remote_pkt_header_t hdr;
    uint8_t screen_type;
} remote_pkt_screen_t;

/* 0x09 眼睛表情 */
typedef struct {
    remote_pkt_header_t hdr;
    uint8_t emotion;
} remote_pkt_eye_emotion_t;

/* 0x0A 虹膜 */
typedef struct {
    remote_pkt_header_t hdr;
    uint8_t iris_type;
} remote_pkt_eye_iris_t;

/* --- 控制指令应答包 [0xFF][cmd_id][status] --- */
#define REMOTE_RSP_MARKER      0xFF

typedef struct {
    uint8_t marker;
    uint8_t cmd_id;
    uint8_t status;
} remote_pkt_response_t;

/* --- 数据查询应答包 [0xFE][cmd_id][payload...] --- */
#define REMOTE_DATA_MARKER     0xFE

/* IR 数据应答 */
typedef struct {
    uint8_t marker;
    uint8_t cmd_id;
    uint8_t counts[4];
} remote_pkt_ir_data_t;

/* IMU 数据应答 */
typedef struct {
    uint8_t marker;
    uint8_t cmd_id;
    float   ax, ay, az;
    float   gx, gy, gz;
} remote_pkt_imu_data_t;

/* 触摸计数数据应答:
 * [0xFE][0x0D][left_click u32][left_dbl u32][left_long u32][left_total u32]
 *             [right_click u32][right_dbl u32][right_long u32][right_total u32]
 * 共 2 + 4*4*2 = 34 字节
 */
typedef struct {
    uint8_t  marker;          // 0xFE
    uint8_t  cmd_id;          // 0x0D
    uint32_t click[2];        // [left, right] 单击
    uint32_t double_click[2]; // [left, right] 双击
    uint32_t long_press[2];   // [left, right] 长按
    uint32_t total[2];        // [left, right] 总计
} remote_pkt_touch_data_t;

#pragma pack(pop)

/* ================================================================
 *  应答联合体 (传输层用于回传)
 * ================================================================ */
typedef struct {
    union {
        remote_pkt_response_t   rsp;
        remote_pkt_ir_data_t    ir;
        remote_pkt_imu_data_t   imu;
        remote_pkt_touch_data_t touch;  /* ← 新增 */
        uint8_t                 raw[48]; /* 扩大以容纳 touch_data */
    } pkt;
    uint16_t len;
} remote_response_t;

/* ================================================================
 *  公开 API
 * ================================================================ */
remote_rsp_code_t remote_protocol_handle(const uint8_t *data, uint16_t len,
                                         remote_response_t *rsp);

#ifdef __cplusplus
}
#endif

#endif /* REMOTE_PROTOCOL_H */