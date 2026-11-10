#ifndef APP_MOTOR_H 
#define APP_MOTOR_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif


/* 宏定义：是否开启电机角度驱动功能 */
#ifndef ENABLE_MOTOR_IMU_CONTROL
#define ENABLE_MOTOR_IMU_CONTROL 1
#endif

/**
 * 宏定义：是否开启 IR 电源联动控制
 *
 * 依赖：需要在项目中包含 ir.h / ir.c，并已完成 ir_init() 初始化。
 * 若不需要此功能，将下方的 1 改为 0 即可。
 */
#ifndef ENABLE_MOTOR_IR_POWER
#define ENABLE_MOTOR_IR_POWER 1
#endif

/* ================= 硬件引脚配置 ================= */
#define APP_MOTOR_PIN_L_A GPIO_NUM_17
#define APP_MOTOR_PIN_L_B GPIO_NUM_18
#define APP_MOTOR_PIN_R_A GPIO_NUM_26
#define APP_MOTOR_PIN_R_B GPIO_NUM_27

/* ================= 参数配置 ================= */
#define APP_MOTOR_MAX_SPEED 100         // 逻辑最大速度 (-100 ~ 100)
#define APP_MOTOR_PWM_FREQ 20000        // PWM 频率 20kHz
#define APP_MOTOR_CTRL_FREQ_HZ 100      // 控制回路频率 100Hz (10ms 周期)

/* 速度斜坡：单位 "速度单位 / 秒"，与控制频率解耦。
 * 这三个值只是上电默认值，可用 app_motor_set_ramp() 在运行时调整，便于实车调参。
 *
 *   ACCEL   起步（静止 → 同向加速）：保守，限制启动冲击电流
 *   DECEL   收油（|大| → 0，不跨零）
 *   REVERSE 换向（跨零反向）：这是 "快速左右打杆" 跟手度的决定性参数。
 *           整段换向（100 → 0 → -100）全程使用此斜率 —— 若过零后就退回 ACCEL，
 *           后半程会被拖慢一倍以上，这正是打杆跟不上的主因。
 *
 * 满量程反转共 200 个速度单位，耗时 = 200 / REVERSE 秒
 */
#define APP_MOTOR_RAMP_ACCEL   1500.0f  //   0 →  100  约 67ms
#define APP_MOTOR_RAMP_DECEL   4000.0f  // 100 →    0  约 25ms
#define APP_MOTOR_RAMP_REVERSE 7000.0f  // 100 → -100  约 29ms

#define APP_MOTOR_DIAG          0       // 1=每 2s 打印控制回路诊断（调完置 0）

#define APP_MOTOR_CMD_TIMEOUT_MS 500    // 遥控模式指令超时时间 (ms)
#define APP_MOTOR_IDLE_TIMEOUT_MS 3000  // 空闲超时时间，超时后进入待机 (ms)
#define APP_MOTOR_QUEUE_SIZE 8          // 消息队列深度
#define APP_MOTOR_TASK_PRIO 12          // 电机任务优先级（需高于 ui_logic 的 10）
#define APP_MOTOR_TASK_STACK 3072       // 电机任务栈深度

/* ================= 指令类型定义 ================= */
typedef enum {
    APP_MOTOR_CMD_REMOTE,  // 遥控模式指令 (油门+转向)
    APP_MOTOR_CMD_AUTO,    // 自动模式指令 (定时定速)
    APP_MOTOR_CMD_ESTOP,   // 紧急停止
    APP_MOTOR_CMD_RESUME,  // 恢复运行
    APP_MOTOR_CMD_STOP,    // 正常停止
#if ENABLE_MOTOR_IMU_CONTROL
    APP_MOTOR_CMD_TURN_ANGLE  // 角度转向指令
#endif
} app_motor_cmd_type_t;

typedef struct {
    app_motor_cmd_type_t type;

    union {
        // 遥控模式参数
        struct {
            float throttle;  // 油门 (-1.0 ~ 1.0)
            float steering;  // 转向 (-1.0 ~ 1.0)
        } remote;

        // 自动模式参数
        struct {
            int16_t speed_l;       // 左轮速度 (-100 ~ 100)
            int16_t speed_r;       // 右轮速度 (-100 ~ 100)
            uint32_t duration_ms;  // 持续时间 (ms)
        } auto_move;

#if ENABLE_MOTOR_IMU_CONTROL
        // 角度转向参数
        struct {
            float target_angle;  // 目标角度增量，单位：度 (+90 或 -90 等)
            int16_t max_speed;   // 最大旋转速度 (1 ~ 100)
        } turn_angle;
#endif
    } params;

} app_motor_cmd_t;

/* ================= API 接口 ================= */
esp_err_t app_motor_init(void);
esp_err_t app_motor_set_remote(float throttle, float steering);
esp_err_t app_motor_move_auto(int16_t speed_l, int16_t speed_r, uint32_t duration_ms);

#if ENABLE_MOTOR_IMU_CONTROL
/**
 * @brief [角度控制模式] 使用IMU闭环控制旋转指定角度
 * @param target_angle_deg 相对旋转角度 (度)，正数为右转/左转
 * @param max_speed 旋转最大速度 (1~100)
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t app_motor_turn_angle(float target_angle_deg, int16_t max_speed);
#endif

esp_err_t app_motor_estop(void);
esp_err_t app_motor_resume(void);
esp_err_t app_motor_stop(void);
bool app_motor_is_active(void);
void app_motor_set_invert(bool inv_l, bool inv_r);

/**
 * @brief 运行时调整速度斜坡（单位：速度单位/秒），用于实车调参，无需重新编译
 * @param accel   起步斜率，<=0 表示保持当前值
 * @param decel   收油斜率，<=0 表示保持当前值
 * @param reverse 换向斜率，<=0 表示保持当前值
 */
void app_motor_set_ramp(float accel, float decel, float reverse);

#ifdef __cplusplus
}
#endif

#endif  // APP_MOTOR_H