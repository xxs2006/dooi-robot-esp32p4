#include "xbox_controller.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "XBOX";

/* 数据包中各字段的字节偏移 */
#define IDX_JOY_LH_LO      0
#define IDX_JOY_LH_HI      1
#define IDX_JOY_LV_LO      2
#define IDX_JOY_LV_HI      3
#define IDX_JOY_RH_LO      4
#define IDX_JOY_RH_HI      5
#define IDX_JOY_RV_LO      6
#define IDX_JOY_RV_HI      7
#define IDX_TRIG_LT_LO     8
#define IDX_TRIG_LT_HI     9
#define IDX_TRIG_RT_LO     10
#define IDX_TRIG_RT_HI     11
#define IDX_BUTTONS_DIR     12
#define IDX_BUTTONS_MAIN    13
#define IDX_BUTTONS_CENTER  14
#define IDX_BUTTONS_SHARE   15

/* 主要按键位掩码 (byte 13) */
#define MASK_BTN_A          0x01
#define MASK_BTN_B          0x02
#define MASK_BTN_X          0x08
#define MASK_BTN_Y          0x10
#define MASK_BTN_LB         0x40
#define MASK_BTN_RB         0x80

/* 中央按键位掩码 (byte 14) */
#define MASK_BTN_SELECT     0x04
#define MASK_BTN_START      0x08
#define MASK_BTN_XBOX       0x10
#define MASK_BTN_LS         0x20
#define MASK_BTN_RS         0x40

/* Share按键位掩码 (byte 15) */
#define MASK_BTN_SHARE      0x01

/* 震动报告select字节位掩码 */
#define VIB_SELECT_CENTER   0x01
#define VIB_SELECT_SHAKE    0x02
#define VIB_SELECT_RIGHT    0x04
#define VIB_SELECT_LEFT     0x08

/* 全局手柄状态实例 */
static xbox_controller_state_t s_xbox_state = {
    .analog = {
        .joy_lh = XBOX_JOY_CENTER,
        .joy_lv = XBOX_JOY_CENTER,
        .joy_rh = XBOX_JOY_CENTER,
        .joy_rv = XBOX_JOY_CENTER,
        .trig_lt = 0,
        .trig_rt = 0,
    },
    .dpad_raw = XBOX_DPAD_NEUTRAL,
    .connected = false,
};

/* 全局HID写入回调 */
static xbox_hid_write_cb_t s_hid_write_cb = NULL;

/* ======================== 辅助函数 ======================== */

static inline uint8_t clamp_power(uint8_t val)
{
    return (val > XBOX_VIBRATION_MAX) ? XBOX_VIBRATION_MAX : val;
}

/* ======================== 解析相关实现 ======================== */
uint8_t xbox_controller_parse(xbox_controller_state_t *state, const uint8_t *data, uint16_t len)
{
    if (state == NULL || data == NULL) {
        return XBOX_ERR_INVALID_LEN;
    }

    if (len != XBOX_DATA_LEN) {
        return XBOX_ERR_INVALID_LEN;
    }

    memcpy(state->raw_data, data, XBOX_DATA_LEN);
    state->connected = true;

    /* 摇杆 (小端序 uint16) */
    state->analog.joy_lh = (uint16_t)data[IDX_JOY_LH_LO] | ((uint16_t)data[IDX_JOY_LH_HI] << 8);
    state->analog.joy_lv = (uint16_t)data[IDX_JOY_LV_LO] | ((uint16_t)data[IDX_JOY_LV_HI] << 8);
    state->analog.joy_rh = (uint16_t)data[IDX_JOY_RH_LO] | ((uint16_t)data[IDX_JOY_RH_HI] << 8);
    state->analog.joy_rv = (uint16_t)data[IDX_JOY_RV_LO] | ((uint16_t)data[IDX_JOY_RV_HI] << 8);

    /* 扳机 */
    state->analog.trig_lt = (uint16_t)data[IDX_TRIG_LT_LO] | ((uint16_t)data[IDX_TRIG_LT_HI] << 8);
    state->analog.trig_rt = (uint16_t)data[IDX_TRIG_RT_LO] | ((uint16_t)data[IDX_TRIG_RT_HI] << 8);

    /* 主要按键 (byte 13) */
    uint8_t btn_main = data[IDX_BUTTONS_MAIN];
    state->buttons.btn_a  = (btn_main & MASK_BTN_A)  ? true : false;
    state->buttons.btn_b  = (btn_main & MASK_BTN_B)  ? true : false;
    state->buttons.btn_x  = (btn_main & MASK_BTN_X)  ? true : false;
    state->buttons.btn_y  = (btn_main & MASK_BTN_Y)  ? true : false;
    state->buttons.btn_lb = (btn_main & MASK_BTN_LB) ? true : false;
    state->buttons.btn_rb = (btn_main & MASK_BTN_RB) ? true : false;

    /* 中央按键 (byte 14) */
    uint8_t btn_center = data[IDX_BUTTONS_CENTER];
    state->buttons.btn_select = (btn_center & MASK_BTN_SELECT) ? true : false;
    state->buttons.btn_start  = (btn_center & MASK_BTN_START)  ? true : false;
    state->buttons.btn_xbox   = (btn_center & MASK_BTN_XBOX)   ? true : false;
    state->buttons.btn_ls     = (btn_center & MASK_BTN_LS)     ? true : false;
    state->buttons.btn_rs     = (btn_center & MASK_BTN_RS)     ? true : false;

    /* Share按键 (byte 15) */
    state->buttons.btn_share = (data[IDX_BUTTONS_SHARE] & MASK_BTN_SHARE) ? true : false;

    /* D-Pad (byte 12) Hat Switch: 0=中立,1=上,2=右上,3=右,4=右下,5=下,6=左下,7=左,8=左上 */
    uint8_t dpad = data[IDX_BUTTONS_DIR];
    state->dpad_raw = (xbox_dpad_dir_t)dpad;
    state->buttons.btn_dir_up    = (dpad == 1 || dpad == 2 || dpad == 8);
    state->buttons.btn_dir_right = (dpad >= 2 && dpad <= 4);
    state->buttons.btn_dir_down  = (dpad >= 4 && dpad <= 6);
    state->buttons.btn_dir_left  = (dpad >= 6 && dpad <= 8);

    return XBOX_OK;
}

uint8_t xbox_controller_encode(const xbox_controller_state_t *state, uint8_t *data, uint16_t len)
{
    if (state == NULL || data == NULL || len < XBOX_DATA_LEN) {
        return XBOX_ERR_INVALID_LEN;
    }

    memset(data, 0, XBOX_DATA_LEN);

    data[IDX_JOY_LH_LO] = state->analog.joy_lh & 0xFF;
    data[IDX_JOY_LH_HI] = (state->analog.joy_lh >> 8) & 0xFF;
    data[IDX_JOY_LV_LO] = state->analog.joy_lv & 0xFF;
    data[IDX_JOY_LV_HI] = (state->analog.joy_lv >> 8) & 0xFF;
    data[IDX_JOY_RH_LO] = state->analog.joy_rh & 0xFF;
    data[IDX_JOY_RH_HI] = (state->analog.joy_rh >> 8) & 0xFF;
    data[IDX_JOY_RV_LO] = state->analog.joy_rv & 0xFF;
    data[IDX_JOY_RV_HI] = (state->analog.joy_rv >> 8) & 0xFF;

    data[IDX_TRIG_LT_LO] = state->analog.trig_lt & 0xFF;
    data[IDX_TRIG_LT_HI] = (state->analog.trig_lt >> 8) & 0xFF;
    data[IDX_TRIG_RT_LO] = state->analog.trig_rt & 0xFF;
    data[IDX_TRIG_RT_HI] = (state->analog.trig_rt >> 8) & 0xFF;

    uint8_t btn_main = 0;
    if (state->buttons.btn_a)  btn_main |= MASK_BTN_A;
    if (state->buttons.btn_b)  btn_main |= MASK_BTN_B;
    if (state->buttons.btn_x)  btn_main |= MASK_BTN_X;
    if (state->buttons.btn_y)  btn_main |= MASK_BTN_Y;
    if (state->buttons.btn_lb) btn_main |= MASK_BTN_LB;
    if (state->buttons.btn_rb) btn_main |= MASK_BTN_RB;
    data[IDX_BUTTONS_MAIN] = btn_main;

    uint8_t btn_center = 0;
    if (state->buttons.btn_select) btn_center |= MASK_BTN_SELECT;
    if (state->buttons.btn_start)  btn_center |= MASK_BTN_START;
    if (state->buttons.btn_xbox)   btn_center |= MASK_BTN_XBOX;
    if (state->buttons.btn_ls)     btn_center |= MASK_BTN_LS;
    if (state->buttons.btn_rs)     btn_center |= MASK_BTN_RS;
    data[IDX_BUTTONS_CENTER] = btn_center;

    data[IDX_BUTTONS_SHARE] = state->buttons.btn_share ? MASK_BTN_SHARE : 0;

    uint8_t dpad = 0;
    if (state->buttons.btn_dir_up) {
        if (state->buttons.btn_dir_right)      dpad = 2;
        else if (state->buttons.btn_dir_left)  dpad = 8;
        else                                   dpad = 1;
    } else if (state->buttons.btn_dir_down) {
        if (state->buttons.btn_dir_right)      dpad = 4;
        else if (state->buttons.btn_dir_left)  dpad = 6;
        else                                   dpad = 5;
    } else {
        if (state->buttons.btn_dir_right)      dpad = 3;
        else if (state->buttons.btn_dir_left)  dpad = 7;
        else                                   dpad = 0;
    }
    data[IDX_BUTTONS_DIR] = dpad;

    return XBOX_OK;
}

void xbox_controller_print_state(const xbox_controller_state_t *state)
{
    if (state == NULL || !state->connected) return;

    char buf[512];
    int offset = 0;
    bool has_content = false;

    if (state->buttons.btn_a)         { offset += snprintf(buf + offset, sizeof(buf) - offset, "A "); has_content = true; }
    if (state->buttons.btn_b)         { offset += snprintf(buf + offset, sizeof(buf) - offset, "B "); has_content = true; }
    if (state->buttons.btn_x)         { offset += snprintf(buf + offset, sizeof(buf) - offset, "X "); has_content = true; }
    if (state->buttons.btn_y)         { offset += snprintf(buf + offset, sizeof(buf) - offset, "Y "); has_content = true; }
    if (state->buttons.btn_lb)        { offset += snprintf(buf + offset, sizeof(buf) - offset, "LB "); has_content = true; }
    if (state->buttons.btn_rb)        { offset += snprintf(buf + offset, sizeof(buf) - offset, "RB "); has_content = true; }
    if (state->buttons.btn_select)    { offset += snprintf(buf + offset, sizeof(buf) - offset, "SEL "); has_content = true; }
    if (state->buttons.btn_start)     { offset += snprintf(buf + offset, sizeof(buf) - offset, "START "); has_content = true; }
    if (state->buttons.btn_xbox)      { offset += snprintf(buf + offset, sizeof(buf) - offset, "XBOX "); has_content = true; }
    if (state->buttons.btn_share)     { offset += snprintf(buf + offset, sizeof(buf) - offset, "SHARE "); has_content = true; }
    if (state->buttons.btn_ls)        { offset += snprintf(buf + offset, sizeof(buf) - offset, "LS "); has_content = true; }
    if (state->buttons.btn_rs)        { offset += snprintf(buf + offset, sizeof(buf) - offset, "RS "); has_content = true; }
    if (state->buttons.btn_dir_up)    { offset += snprintf(buf + offset, sizeof(buf) - offset, "UP "); has_content = true; }
    if (state->buttons.btn_dir_down)  { offset += snprintf(buf + offset, sizeof(buf) - offset, "DOWN "); has_content = true; }
    if (state->buttons.btn_dir_left)  { offset += snprintf(buf + offset, sizeof(buf) - offset, "LEFT "); has_content = true; }
    if (state->buttons.btn_dir_right) { offset += snprintf(buf + offset, sizeof(buf) - offset, "RIGHT "); has_content = true; }

    int32_t lh_diff = (int32_t)state->analog.joy_lh - XBOX_JOY_CENTER;
    int32_t lv_diff = (int32_t)state->analog.joy_lv - XBOX_JOY_CENTER;
    int32_t rh_diff = (int32_t)state->analog.joy_rh - XBOX_JOY_CENTER;
    int32_t rv_diff = (int32_t)state->analog.joy_rv - XBOX_JOY_CENTER;

    if (abs(lh_diff) > XBOX_JOY_DEADZONE || abs(lv_diff) > XBOX_JOY_DEADZONE) {
        offset += snprintf(buf + offset, sizeof(buf) - offset, "| LJ(%u,%u) ",
                          state->analog.joy_lh, state->analog.joy_lv);
        has_content = true;
    }
    if (abs(rh_diff) > XBOX_JOY_DEADZONE || abs(rv_diff) > XBOX_JOY_DEADZONE) {
        offset += snprintf(buf + offset, sizeof(buf) - offset, "| RJ(%u,%u) ",
                          state->analog.joy_rh, state->analog.joy_rv);
        has_content = true;
    }

    if (state->analog.trig_lt > 0) {
        offset += snprintf(buf + offset, sizeof(buf) - offset, "| LT:%u ", state->analog.trig_lt);
        has_content = true;
    }
    if (state->analog.trig_rt > 0) {
        offset += snprintf(buf + offset, sizeof(buf) - offset, "| RT:%u ", state->analog.trig_rt);
        has_content = true;
    }

    if (has_content) {
        ESP_LOGI(TAG, "%s", buf);
    }
}

void xbox_controller_print_all(const xbox_controller_state_t *state)
{
    if (state == NULL) return;

    ESP_LOGI(TAG, "=== Xbox Controller State ===");
    ESP_LOGI(TAG, "Buttons: A=%d B=%d X=%d Y=%d LB=%d RB=%d",
             state->buttons.btn_a, state->buttons.btn_b,
             state->buttons.btn_x, state->buttons.btn_y,
             state->buttons.btn_lb, state->buttons.btn_rb);
    ESP_LOGI(TAG, "Center:  SEL=%d START=%d XBOX=%d SHARE=%d LS=%d RS=%d",
             state->buttons.btn_select, state->buttons.btn_start,
             state->buttons.btn_xbox, state->buttons.btn_share,
             state->buttons.btn_ls, state->buttons.btn_rs);
    ESP_LOGI(TAG, "D-Pad:   UP=%d DOWN=%d LEFT=%d RIGHT=%d (raw=%d)",
             state->buttons.btn_dir_up, state->buttons.btn_dir_down,
             state->buttons.btn_dir_left, state->buttons.btn_dir_right,
             state->dpad_raw);
    ESP_LOGI(TAG, "L-Joy:   H=%u V=%u", state->analog.joy_lh, state->analog.joy_lv);
    ESP_LOGI(TAG, "R-Joy:   H=%u V=%u", state->analog.joy_rh, state->analog.joy_rv);
    ESP_LOGI(TAG, "Trigger: LT=%u RT=%u", state->analog.trig_lt, state->analog.trig_rt);
}

bool xbox_joy_left_active(const xbox_controller_state_t *state)
{
    if (state == NULL) return false;
    int32_t lh_diff = (int32_t)state->analog.joy_lh - XBOX_JOY_CENTER;
    int32_t lv_diff = (int32_t)state->analog.joy_lv - XBOX_JOY_CENTER;
    return (abs(lh_diff) > XBOX_JOY_DEADZONE) || (abs(lv_diff) > XBOX_JOY_DEADZONE);
}

bool xbox_joy_right_active(const xbox_controller_state_t *state)
{
    if (state == NULL) return false;
    int32_t rh_diff = (int32_t)state->analog.joy_rh - XBOX_JOY_CENTER;
    int32_t rv_diff = (int32_t)state->analog.joy_rv - XBOX_JOY_CENTER;
    return (abs(rh_diff) > XBOX_JOY_DEADZONE) || (abs(rv_diff) > XBOX_JOY_DEADZONE);
}

xbox_controller_state_t* xbox_controller_get_state(void)
{
    return &s_xbox_state;
}

void xbox_controller_on_data(uint8_t *data, uint16_t len)
{
    uint8_t ret = xbox_controller_parse(&s_xbox_state, data, len);
    if (ret == XBOX_OK) {
        xbox_controller_print_state(&s_xbox_state);
    } else {
        ESP_LOGW(TAG, "Xbox parse error: invalid len %u (expected %d)", len, XBOX_DATA_LEN);
    }
}

/* ======================== 震动相关实现 ======================== */

void xbox_vibration_register_write_cb(xbox_hid_write_cb_t cb)
{
    s_hid_write_cb = cb;
}

void xbox_vibration_init(xbox_vibration_t *vib)
{
    if (vib == NULL) return;
    memset(vib, 0, sizeof(xbox_vibration_t));
}

void xbox_vibration_encode(const xbox_vibration_t *vib, uint8_t *data)
{
    if (vib == NULL || data == NULL) return;

    /*
     * 震动HID报告格式 (8字节):
     * Byte 0: select (位域: bit0=center, bit1=shake, bit2=right, bit3=left)
     * Byte 1: left trigger motor power (0-100)
     * Byte 2: right trigger motor power (0-100)
     * Byte 3: shake/main motor power (0-100)
     * Byte 4: center motor power (0-100)
     * Byte 5: time_active (值 * 10ms)
     * Byte 6: time_silent (值 * 10ms)
     * Byte 7: count_repeat
     */
    uint8_t select_byte = 0;
    if (vib->select.center) select_byte |= VIB_SELECT_CENTER;
    if (vib->select.shake)  select_byte |= VIB_SELECT_SHAKE;
    if (vib->select.right)  select_byte |= VIB_SELECT_RIGHT;
    if (vib->select.left)   select_byte |= VIB_SELECT_LEFT;

    data[0] = select_byte;
    data[1] = clamp_power(vib->power.left);
    data[2] = clamp_power(vib->power.right);
    data[3] = clamp_power(vib->power.shake);
    data[4] = clamp_power(vib->power.center);
    data[5] = vib->time_active;
    data[6] = vib->time_silent;
    data[7] = vib->count_repeat;
}

uint8_t xbox_vibration_send(const xbox_vibration_t *vib)
{
    if (s_hid_write_cb == NULL) {
        ESP_LOGW(TAG, "Vibration write callback not registered!");
        return XBOX_ERR_NO_CALLBACK;
    }
    if (vib == NULL) {
        return XBOX_ERR_INVALID_LEN;
    }

    uint8_t report[XBOX_VIBRATION_LEN];
    xbox_vibration_encode(vib, report);

    ESP_LOGD(TAG, "Vibration send: %02X %02X %02X %02X %02X %02X %02X %02X",
             report[0], report[1], report[2], report[3],
             report[4], report[5], report[6], report[7]);

    return s_hid_write_cb(report, XBOX_VIBRATION_LEN);
}

uint8_t xbox_vibration_stop(void)
{
    xbox_vibration_t vib;
    xbox_vibration_init(&vib);

    /* 选择所有马达，力度设0 */
    vib.select.center = true;
    vib.select.shake  = true;
    vib.select.left   = true;
    vib.select.right  = true;
    vib.power.center  = 0;
    vib.power.shake   = 0;
    vib.power.left    = 0;
    vib.power.right   = 0;
    vib.time_active   = 0;
    vib.time_silent   = 0;
    vib.count_repeat  = 0;

    return xbox_vibration_send(&vib);
}

uint8_t xbox_vibration_pulse(uint8_t power, uint8_t duration)
{
    xbox_vibration_t vib;
    xbox_vibration_init(&vib);

    vib.select.center = true;
    vib.select.shake  = true;
    vib.select.left   = true;
    vib.select.right  = true;

    uint8_t p = clamp_power(power);
    vib.power.center = p;
    vib.power.shake  = p;
    vib.power.left   = p;
    vib.power.right  = p;
    vib.time_active  = duration;
    vib.time_silent  = 0;
    vib.count_repeat = 0;

    return xbox_vibration_send(&vib);
}

uint8_t xbox_vibration_rumble(uint8_t left_power, uint8_t right_power, uint8_t duration)
{
    xbox_vibration_t vib;
    xbox_vibration_init(&vib);

    /* 主震动用center和shake马达模拟左右手柄震动 */
    vib.select.center = true;
    vib.select.shake  = true;

    vib.power.center = clamp_power(left_power);
    vib.power.shake  = clamp_power(right_power);
    vib.time_active  = duration;
    vib.time_silent  = 0;
    vib.count_repeat = 0;

    return xbox_vibration_send(&vib);
}

uint8_t xbox_vibration_trigger(uint8_t left_power, uint8_t right_power, uint8_t duration)
{
    xbox_vibration_t vib;
    xbox_vibration_init(&vib);

    vib.select.left  = true;
    vib.select.right = true;

    vib.power.left   = clamp_power(left_power);
    vib.power.right  = clamp_power(right_power);
    vib.time_active  = duration;
    vib.time_silent  = 0;
    vib.count_repeat = 0;

    return xbox_vibration_send(&vib);
}

uint8_t xbox_vibration_by_trigger(uint16_t trigger_value)
{
    if (trigger_value == 0) {
        return xbox_vibration_stop();
    }

    /* 将扳机值(0~1023)映射到力度(0~100) */
    uint8_t power = (uint8_t)((float)trigger_value / (float)XBOX_TRIG_MAX * 100.0f);
    if (power > 100) power = 100;

    xbox_vibration_t vib;
    xbox_vibration_init(&vib);

    vib.select.center = true;
    vib.select.shake  = true;
    vib.select.left   = true;
    vib.select.right  = true;

    vib.power.center = power;
    vib.power.shake  = power;
    vib.power.left   = power / 2;  // 扳机马达力度减半，体验更好
    vib.power.right  = power / 2;
    vib.time_active  = 10;   // 100ms
    vib.time_silent  = 0;
    vib.count_repeat = 0;

    return xbox_vibration_send(&vib);
}