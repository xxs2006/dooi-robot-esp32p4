#ifndef __XBOX_CONTROLLER_H__
#define __XBOX_CONTROLLER_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 数据包长度 */
#define XBOX_DATA_LEN           16

/* 震动报告长度 */
#define XBOX_VIBRATION_LEN      8

/* 摇杆最大值 */
#define XBOX_JOY_MAX            0xFFFF
#define XBOX_JOY_CENTER         (XBOX_JOY_MAX / 2)

/* 扳机最大值 */
#define XBOX_TRIG_MAX           0x03FF

/* 摇杆死区 */
#define XBOX_JOY_DEADZONE       1500

/* 震动力度最大值 (0~100) */
#define XBOX_VIBRATION_MAX      100

/* 错误码 */
#define XBOX_OK                 0
#define XBOX_ERR_INVALID_LEN    1
#define XBOX_ERR_NO_CALLBACK    2

/**
 * @brief D-Pad方向枚举
 */
typedef enum {
    XBOX_DPAD_NEUTRAL    = 0,
    XBOX_DPAD_UP         = 1,
    XBOX_DPAD_UP_RIGHT   = 2,
    XBOX_DPAD_RIGHT      = 3,
    XBOX_DPAD_DOWN_RIGHT = 4,
    XBOX_DPAD_DOWN       = 5,
    XBOX_DPAD_DOWN_LEFT  = 6,
    XBOX_DPAD_LEFT       = 7,
    XBOX_DPAD_UP_LEFT    = 8,
} xbox_dpad_dir_t;

/**
 * @brief Xbox手柄按键状态
 */
typedef struct {
    bool btn_a;
    bool btn_b;
    bool btn_x;
    bool btn_y;
    bool btn_lb;
    bool btn_rb;
    bool btn_select;
    bool btn_start;
    bool btn_xbox;
    bool btn_share;
    bool btn_ls;
    bool btn_rs;
    bool btn_dir_up;
    bool btn_dir_down;
    bool btn_dir_left;
    bool btn_dir_right;
} xbox_buttons_t;

/**
 * @brief Xbox手柄模拟量状态
 */
typedef struct {
    uint16_t joy_lh;
    uint16_t joy_lv;
    uint16_t joy_rh;
    uint16_t joy_rv;
    uint16_t trig_lt;
    uint16_t trig_rt;
} xbox_analog_t;

/**
 * @brief 震动马达选择
 */
typedef struct {
    bool center;    // 中央马达（手柄底部）
    bool shake;     // 震动马达（主震动）
    bool left;      // 左扳机马达
    bool right;     // 右扳机马达
} xbox_vibration_select_t;

/**
 * @brief 震动力度 (0~100)
 */
typedef struct {
    uint8_t left;       // 左扳机马达力度
    uint8_t right;      // 右扳机马达力度
    uint8_t shake;      // 主震动马达力度
    uint8_t center;     // 中央马达力度
} xbox_vibration_power_t;

/**
 * @brief 震动报告结构
 */
typedef struct {
    xbox_vibration_select_t select;     // 选择激活哪些马达
    xbox_vibration_power_t  power;      // 各马达力度 (0-100)
    uint8_t time_active;                // 激活时间 (值 * 10ms, 最大2.55秒)
    uint8_t time_silent;                // 静默时间 (值 * 10ms)
    uint8_t count_repeat;               // 重复次数 (0=单次, 0xFF=最大)
} xbox_vibration_t;

/**
 * @brief Xbox手柄完整状态结构体
 */
typedef struct {
    xbox_buttons_t  buttons;
    xbox_analog_t   analog;
    xbox_dpad_dir_t dpad_raw;
    uint8_t         raw_data[XBOX_DATA_LEN];
    bool            connected;
} xbox_controller_state_t;

/**
 * @brief HID写入回调函数类型
 * @param data 要发送的数据
 * @param len  数据长度
 * @return 0成功，其他失败
 */
typedef uint8_t (*xbox_hid_write_cb_t)(uint8_t *data, uint16_t len);

/* ======================== 解析相关 API ======================== */
uint8_t xbox_controller_parse(xbox_controller_state_t *state, const uint8_t *data, uint16_t len);
uint8_t xbox_controller_encode(const xbox_controller_state_t *state, uint8_t *data, uint16_t len);
void xbox_controller_print_state(const xbox_controller_state_t *state);
void xbox_controller_print_all(const xbox_controller_state_t *state);
bool xbox_joy_left_active(const xbox_controller_state_t *state);
bool xbox_joy_right_active(const xbox_controller_state_t *state);
xbox_controller_state_t* xbox_controller_get_state(void);
void xbox_controller_on_data(uint8_t *data, uint16_t len);

/* ======================== 震动相关 API ======================== */

/**
 * @brief 注册HID写入回调（发送震动命令用）
 * @param cb 写入回调函数
 */
void xbox_vibration_register_write_cb(xbox_hid_write_cb_t cb);

/**
 * @brief 初始化震动结构（全部清零/关闭）
 * @param vib 震动结构指针
 */
void xbox_vibration_init(xbox_vibration_t *vib);

/**
 * @brief 将震动结构编码为8字节报告
 * @param vib  震动结构
 * @param data 输出缓冲区（至少8字节）
 */
void xbox_vibration_encode(const xbox_vibration_t *vib, uint8_t *data);

/**
 * @brief 发送自定义震动
 * @param vib 震动参数
 * @return XBOX_OK或错误码
 */
uint8_t xbox_vibration_send(const xbox_vibration_t *vib);

/**
 * @brief 停止所有震动
 * @return XBOX_OK或错误码
 */
uint8_t xbox_vibration_stop(void);

/**
 * @brief 全马达短震（按键反馈用）
 * @param power    力度 (0~100)
 * @param duration 持续时间，单位10ms (例如50=500ms)
 * @return XBOX_OK或错误码
 */
uint8_t xbox_vibration_pulse(uint8_t power, uint8_t duration);

/**
 * @brief 左右马达震动（类似传统手柄）
 * @param left_power  左马达力度 (0~100)
 * @param right_power 右马达力度 (0~100)
 * @param duration    持续时间，单位10ms
 * @return XBOX_OK或错误码
 */
uint8_t xbox_vibration_rumble(uint8_t left_power, uint8_t right_power, uint8_t duration);

/**
 * @brief 扳机震动（用于模拟扳机阻力反馈）
 * @param left_power  左扳机马达力度 (0~100)
 * @param right_power 右扳机马达力度 (0~100)
 * @param duration    持续时间，单位10ms
 * @return XBOX_OK或错误码
 */
uint8_t xbox_vibration_trigger(uint8_t left_power, uint8_t right_power, uint8_t duration);

/**
 * @brief 根据扳机按压程度自动震动（参考代码中的trigRT功能）
 * @param trigger_value 当前扳机值 (0~1023)
 * @return XBOX_OK或错误码
 */
uint8_t xbox_vibration_by_trigger(uint16_t trigger_value);

#ifdef __cplusplus
}
#endif

#endif /* __XBOX_CONTROLLER_H__ */