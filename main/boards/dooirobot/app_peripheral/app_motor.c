#include "app_motor.h" 
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dooi_nvs.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#if ENABLE_MOTOR_IMU_CONTROL
#include "app_imu.h"
#endif

#if ENABLE_MOTOR_IR_POWER
#include "ir.h"
#include "app_ui_logic.h"
#endif

static const char* TAG = "APP_MOTOR";

#define MOTOR_PWM_MIN_DUTY 520  // 约 60% of 1023

/* ================= PWM 硬件配置 ================= */
#define MOTOR_LEDC_TIMER LEDC_TIMER_2
#define MOTOR_LEDC_MODE LEDC_LOW_SPEED_MODE
#define MOTOR_LEDC_RESOLUTION LEDC_TIMER_10_BIT
#define MOTOR_PWM_MAX_DUTY 1023

#define MOTOR_CH_L_A LEDC_CHANNEL_6
#define MOTOR_CH_L_B LEDC_CHANNEL_1
#define MOTOR_CH_R_A LEDC_CHANNEL_2
#define MOTOR_CH_R_B LEDC_CHANNEL_3

/* ================= 内部状态定义 ================= */
typedef enum { MOTOR_STATE_STANDBY, MOTOR_STATE_ACTIVE, MOTOR_STATE_ESTOP } motor_state_t;

typedef enum {
    MOTOR_EXEC_IDLE,
    MOTOR_EXEC_REMOTE,
    MOTOR_EXEC_AUTO,
#if ENABLE_MOTOR_IMU_CONTROL
    MOTOR_EXEC_TURN_ANGLE
#endif
} motor_exec_mode_t;

typedef struct {
    QueueHandle_t cmd_queue;

    motor_state_t state;
    motor_exec_mode_t exec_mode;
    volatile bool is_active;

    float current_speed_l;
    float current_speed_r;

    /* 换向锁存：跨零反向期间全程使用 REVERSE 斜率，直到逼近目标才解除 */
    bool reversing_l;
    bool reversing_r;

    /* 斜坡参数（可在运行时通过 app_motor_set_ramp() 调整） */
    float ramp_accel;
    float ramp_decel;
    float ramp_reverse;

    float remote_target_l;
    float remote_target_r;
    int64_t last_remote_time_us;

#if APP_MOTOR_DIAG
    struct {
        uint32_t cmd_count;    /* 本统计窗口内收到的指令数 */
        uint32_t tick_count;   /* 本统计窗口内的控制周期数 */
        float    dt_max_ms;    /* 窗口内最大周期间隔（抖动） */
        int64_t  win_start_us;
    } diag;
#endif

    /* auto 模式：以斜坡逼近目标速度，跑满 duration_ms 后结束 */
    struct {
        float target_l, target_r;
        int64_t start_time_us;
        uint32_t duration_ms;
    } auto_params;

#if ENABLE_MOTOR_IMU_CONTROL
    struct {
        float target_angle;
        int16_t max_speed;
        bool is_imu_required;
        int64_t start_time_us;
    } turn_params;
#endif

    bool inv_l;
    bool inv_r;

} motor_context_t;

static motor_context_t g_motor_ctx = {0};

/* ================= IR 电源控制辅助函数 =================
 * 采用边沿触发：只有状态真正翻转时才投递事件。
 * 遥控指令可能以几十~上百 Hz 下发，若每条指令都投一次事件，
 * 电机任务会频繁阻塞在 app_ui_logic_post()（内部有 10ms 阻塞等待），
 * 直接拖慢控制回路；同时 ui_logic 队列(深度仅 5)会被 IR 事件灌满。
 */
#if ENABLE_MOTOR_IR_POWER
/* 初值 true：ir_init() 结束后电源默认是开的，保证首次进入待机仍能真正下发关闭事件 */
static bool s_motor_ir_on = true;
#endif

static void motor_ir_set(bool on)
{
#if ENABLE_MOTOR_IR_POWER
    if (s_motor_ir_on == on) return;
    s_motor_ir_on = on;
    app_event_t evt = {.type = on ? APP_EVT_IR_ENABLE : APP_EVT_IR_DISABLE};
    app_ui_logic_post(&evt, false);
#else
    (void)on;
#endif
}

static inline void motor_ir_on(void)  { motor_ir_set(true);  }
static inline void motor_ir_off(void) { motor_ir_set(false); }

/* ================= 硬件操作函数 ================= */
static inline int64_t motor_get_time_us(void) { return esp_timer_get_time(); }

static esp_err_t motor_hw_init(void) {
    ledc_timer_config_t timer_cfg = {.speed_mode = MOTOR_LEDC_MODE,
                                     .timer_num = MOTOR_LEDC_TIMER,
                                     .duty_resolution = MOTOR_LEDC_RESOLUTION,
                                     .freq_hz = APP_MOTOR_PWM_FREQ,
                                     .clk_cfg = LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t ch_configs[] = {
        {.channel = MOTOR_CH_L_A, .gpio_num = APP_MOTOR_PIN_L_A,
         .speed_mode = MOTOR_LEDC_MODE, .timer_sel = MOTOR_LEDC_TIMER, .duty = 0, .hpoint = 0},
        {.channel = MOTOR_CH_L_B, .gpio_num = APP_MOTOR_PIN_L_B,
         .speed_mode = MOTOR_LEDC_MODE, .timer_sel = MOTOR_LEDC_TIMER, .duty = 0, .hpoint = 0},
        {.channel = MOTOR_CH_R_A, .gpio_num = APP_MOTOR_PIN_R_A,
         .speed_mode = MOTOR_LEDC_MODE, .timer_sel = MOTOR_LEDC_TIMER, .duty = 0, .hpoint = 0},
        {.channel = MOTOR_CH_R_B, .gpio_num = APP_MOTOR_PIN_R_B,
         .speed_mode = MOTOR_LEDC_MODE, .timer_sel = MOTOR_LEDC_TIMER, .duty = 0, .hpoint = 0}
    };
    for (int i = 0; i < 4; i++) {
        ESP_ERROR_CHECK(ledc_channel_config(&ch_configs[i]));
    }
    return ESP_OK;
}

static void motor_hw_set_pwm(int16_t speed, ledc_channel_t ch_a, ledc_channel_t ch_b) {
    if (speed > APP_MOTOR_MAX_SPEED)  speed = APP_MOTOR_MAX_SPEED;
    if (speed < -APP_MOTOR_MAX_SPEED) speed = -APP_MOTOR_MAX_SPEED;
    uint32_t duty = (uint32_t)(abs(speed) * MOTOR_PWM_MAX_DUTY / APP_MOTOR_MAX_SPEED);
    if (duty > 0) duty = MOTOR_PWM_MIN_DUTY + duty * (MOTOR_PWM_MAX_DUTY - MOTOR_PWM_MIN_DUTY) / MOTOR_PWM_MAX_DUTY;
    if (speed >= 0) {
        ledc_set_duty(MOTOR_LEDC_MODE, ch_a, duty);
        ledc_set_duty(MOTOR_LEDC_MODE, ch_b, 0);
    } else {
        ledc_set_duty(MOTOR_LEDC_MODE, ch_a, 0);
        ledc_set_duty(MOTOR_LEDC_MODE, ch_b, duty);
    }
    ledc_update_duty(MOTOR_LEDC_MODE, ch_a);
    ledc_update_duty(MOTOR_LEDC_MODE, ch_b);
}

static void motor_hw_update_output(void) {
    float final_l = g_motor_ctx.current_speed_l * (g_motor_ctx.inv_l ? -1.0f : 1.0f);
    float final_r = g_motor_ctx.current_speed_r * (g_motor_ctx.inv_r ? -1.0f : 1.0f);

    motor_hw_set_pwm((int16_t)final_l,  MOTOR_CH_L_A, MOTOR_CH_L_B);
    motor_hw_set_pwm((int16_t)-final_r, MOTOR_CH_R_A, MOTOR_CH_R_B);
}

static void motor_hw_stop(void) {
    g_motor_ctx.current_speed_l = 0;
    g_motor_ctx.current_speed_r = 0;
    g_motor_ctx.remote_target_l = 0;
    g_motor_ctx.remote_target_r = 0;
    g_motor_ctx.reversing_l    = false;
    g_motor_ctx.reversing_r    = false;
    motor_hw_update_output();
}

/**
 * @brief 按时间积分的速度斜坡（带换向锁存）
 *
 * @param cur      当前速度
 * @param target   目标速度
 * @param dt_s     距上一次控制周期的实际间隔（秒）
 * @param reversing 换向锁存标志（按轮独立，跨周期保持）
 *
 * 斜率选择：
 *   - 跨零反向 → 置位 reversing，整段换向（含过零后的加速段）都用 REVERSE 陡斜率
 *   - 同向收油（|target| < |cur|）→ DECEL
 *   - 同向起步 / 提速         → ACCEL（限制启动冲击电流）
 *
 * 关键：若不加锁存，"100 → -100" 会在过零瞬间从 DECEL 退回 ACCEL，
 * 后半程（真正出力的那一段）被拖慢一倍以上，快速打杆就跟不上。
 */
static float motor_approach(float cur, float target, float dt_s, bool *reversing) {
    if (dt_s <= 0.0f) return cur;

    float diff = target - cur;
    if (fabsf(diff) < 0.5f) { *reversing = false; return target; }

    /* 目标与当前反向 → 进入换向；到达目标后由上面的吸附分支解锁 */
    if (cur * target < 0.0f) *reversing = true;

    float rate;
    if (*reversing)                          rate = g_motor_ctx.ramp_reverse;
    else if (fabsf(target) < fabsf(cur))     rate = g_motor_ctx.ramp_decel;
    else                                     rate = g_motor_ctx.ramp_accel;

    float step = rate * dt_s;
    if (fabsf(diff) <= step) { *reversing = false; return target; }
    return cur + ((diff > 0.0f) ? step : -step);
}

static void motor_mix_arcade(float throttle, float steering, float* out_l, float* out_r) {
    if (throttle >  1.0f) throttle =  1.0f;
    if (throttle < -1.0f) throttle = -1.0f;
    if (steering >  1.0f) steering =  1.0f;
    if (steering < -1.0f) steering = -1.0f;
    float left  = throttle + steering;
    float right = throttle - steering;
    float max_val = fmaxf(fabsf(left), fabsf(right));
    if (max_val > 1.0f) { left /= max_val; right /= max_val; }
    *out_l = left  * APP_MOTOR_MAX_SPEED;
    *out_r = right * APP_MOTOR_MAX_SPEED;
}

static void motor_cleanup_previous_mode(void) {
#if ENABLE_MOTOR_IMU_CONTROL
    if (g_motor_ctx.exec_mode == MOTOR_EXEC_TURN_ANGLE &&
        g_motor_ctx.turn_params.is_imu_required) {
        app_imu_require(false);
        g_motor_ctx.turn_params.is_imu_required = false;
    }
#endif
}

static void motor_exec_remote_tick(float dt_s) {
    int64_t now = motor_get_time_us();
    if ((now - g_motor_ctx.last_remote_time_us) > (APP_MOTOR_CMD_TIMEOUT_MS * 1000)) {
        g_motor_ctx.remote_target_l = 0;
        g_motor_ctx.remote_target_r = 0;
    }
    g_motor_ctx.current_speed_l = motor_approach(
        g_motor_ctx.current_speed_l, g_motor_ctx.remote_target_l, dt_s,
        &g_motor_ctx.reversing_l);
    g_motor_ctx.current_speed_r = motor_approach(
        g_motor_ctx.current_speed_r, g_motor_ctx.remote_target_r, dt_s,
        &g_motor_ctx.reversing_r);
    motor_hw_update_output();
}

/**
 * @brief auto 模式 tick
 * @return true  duration 到期，切回 REMOTE/IDLE
 * @return false 继续执行
 */
static bool motor_exec_auto_tick(float dt_s) {
    g_motor_ctx.current_speed_l = motor_approach(
        g_motor_ctx.current_speed_l,
        g_motor_ctx.auto_params.target_l, dt_s,
        &g_motor_ctx.reversing_l);
    g_motor_ctx.current_speed_r = motor_approach(
        g_motor_ctx.current_speed_r,
        g_motor_ctx.auto_params.target_r, dt_s,
        &g_motor_ctx.reversing_r);
    motor_hw_update_output();

    int64_t elapsed_ms =
        (motor_get_time_us() - g_motor_ctx.auto_params.start_time_us) / 1000;
    return (elapsed_ms >= (int64_t)g_motor_ctx.auto_params.duration_ms);
}

#if ENABLE_MOTOR_IMU_CONTROL
/**
 * @brief 转角控制 tick
 *
 * 约定：error > 0 → 右转（右轮前进、左轮后退）
 *       error < 0 → 左转（左轮前进、右轮后退）
 *
 * 偏航角正方向需与此一致：车体右转时 app_imu_get_yaw() 递增。
 * 如果实测右转时 yaw 递减，请在 app_imu.c 中将 gz = -phys->gz 改为 gz = +phys->gz。
 *
 * @return true  转向完成或超时
 * @return false 继续执行
 */
static bool motor_exec_turn_angle_tick(void) {
    float current_yaw = app_imu_get_yaw();
    float error       = g_motor_ctx.turn_params.target_angle - current_yaw;
    int64_t now       = motor_get_time_us();

    /* 到位判定：误差 < 2° 或超时 10s */
    if (fabsf(error) < 2.0f ||
        (now - g_motor_ctx.turn_params.start_time_us) > 10000000LL)
    {
        g_motor_ctx.current_speed_l = 0;
        g_motor_ctx.current_speed_r = 0;
        motor_hw_update_output();
        return true;
    }

    float output_speed = fabsf((float)g_motor_ctx.turn_params.max_speed);
    if (fabsf(error) < 15.0f) output_speed *= 0.6f;

    const float min_motor_power = 40.0f;
    if (output_speed < min_motor_power) output_speed = min_motor_power;

    if (error < 0) {
        g_motor_ctx.current_speed_l =  output_speed;
        g_motor_ctx.current_speed_r = -output_speed;
    } else {
        g_motor_ctx.current_speed_l = -output_speed;
        g_motor_ctx.current_speed_r =  output_speed;
    }
    motor_hw_update_output();
    return false;
}
#endif

static bool motor_process_cmd(const app_motor_cmd_t* cmd) {
#if APP_MOTOR_DIAG
    g_motor_ctx.diag.cmd_count++;
#endif
    switch (cmd->type) {
        case APP_MOTOR_CMD_REMOTE: {
            motor_cleanup_previous_mode();
            float target_l, target_r;
            motor_mix_arcade(cmd->params.remote.throttle, cmd->params.remote.steering,
                             &target_l, &target_r);
            g_motor_ctx.remote_target_l     = target_l;
            g_motor_ctx.remote_target_r     = target_r;
            g_motor_ctx.last_remote_time_us = motor_get_time_us();
            g_motor_ctx.exec_mode = MOTOR_EXEC_REMOTE;
            motor_ir_off(); /* 遥控模式：IR 保持关闭 */
            return true;
        }
        case APP_MOTOR_CMD_AUTO: {
            motor_cleanup_previous_mode();
            g_motor_ctx.auto_params.target_l      = cmd->params.auto_move.speed_l;
            g_motor_ctx.auto_params.target_r      = cmd->params.auto_move.speed_r;
            g_motor_ctx.auto_params.duration_ms   = cmd->params.auto_move.duration_ms;
            g_motor_ctx.auto_params.start_time_us = motor_get_time_us();
            g_motor_ctx.exec_mode = MOTOR_EXEC_AUTO;
            motor_ir_on(); /* 自动模式：开启 IR */
            return true;
        }
        case APP_MOTOR_CMD_ESTOP:
            motor_cleanup_previous_mode();
            g_motor_ctx.state = MOTOR_STATE_ESTOP;
            motor_hw_stop();
            motor_ir_off();
            return false;
        case APP_MOTOR_CMD_RESUME:
            if (g_motor_ctx.state == MOTOR_STATE_ESTOP) {
                g_motor_ctx.state = MOTOR_STATE_STANDBY;
            }
            return false;
        case APP_MOTOR_CMD_STOP:
            motor_cleanup_previous_mode();
            g_motor_ctx.remote_target_l = 0;
            g_motor_ctx.remote_target_r = 0;
            g_motor_ctx.exec_mode = MOTOR_EXEC_REMOTE;
            motor_ir_off(); /* 停止：关闭 IR */
            return true;
#if ENABLE_MOTOR_IMU_CONTROL
        case APP_MOTOR_CMD_TURN_ANGLE: {
            motor_cleanup_previous_mode();
            g_motor_ctx.turn_params.target_angle = cmd->params.turn_angle.target_angle;
            g_motor_ctx.turn_params.max_speed    = cmd->params.turn_angle.max_speed;
            if (g_motor_ctx.turn_params.max_speed < 10)
                g_motor_ctx.turn_params.max_speed = 10;

            app_imu_require(true);
            app_imu_reset_yaw();

            g_motor_ctx.turn_params.is_imu_required = true;
            g_motor_ctx.turn_params.start_time_us   = motor_get_time_us();
            g_motor_ctx.exec_mode = MOTOR_EXEC_TURN_ANGLE;
            motor_ir_on(); /* 转角模式：开启 IR */
            return true;
        }
#endif
        default:
            return false;
    }
}

#if APP_MOTOR_DIAG
/* 每 APP_MOTOR_DIAG_PERIOD_MS 打印一次控制回路健康状况。
 * 用途：区分 "指令来得慢" 和 "斜坡太肉" —— 看 cmd_rate 与 tick_rate 即可。 */
#define APP_MOTOR_DIAG_PERIOD_MS 2000

static void motor_diag_tick(int64_t now_us, float dt_s)
{
    g_motor_ctx.diag.tick_count++;
    float dt_ms = dt_s * 1000.0f;
    if (dt_ms > g_motor_ctx.diag.dt_max_ms) g_motor_ctx.diag.dt_max_ms = dt_ms;

    int64_t win_us = (int64_t)APP_MOTOR_DIAG_PERIOD_MS * 1000;
    if ((now_us - g_motor_ctx.diag.win_start_us) < win_us) return;

    float secs = (float)(now_us - g_motor_ctx.diag.win_start_us) / 1000000.0f;
    ESP_LOGI(TAG,
             "diag: tick=%.0fHz cmd=%.0f/s dt_max=%.1fms | L cur=%6.1f tgt=%6.1f | R cur=%6.1f tgt=%6.1f",
             (float)g_motor_ctx.diag.tick_count / secs,
             (float)g_motor_ctx.diag.cmd_count  / secs,
             g_motor_ctx.diag.dt_max_ms,
             g_motor_ctx.current_speed_l, g_motor_ctx.remote_target_l,
             g_motor_ctx.current_speed_r, g_motor_ctx.remote_target_r);

    g_motor_ctx.diag.tick_count   = 0;
    g_motor_ctx.diag.cmd_count    = 0;
    g_motor_ctx.diag.dt_max_ms    = 0.0f;
    g_motor_ctx.diag.win_start_us = now_us;
}
#endif /* APP_MOTOR_DIAG */

static void motor_task(void* arg) {
    app_motor_cmd_t cmd;
    /* 先乘后除并四舍五入，避免低频配置下 pdMS_TO_TICKS(整数) 取整后周期偏长 */
    const TickType_t ctrl_period =
        (TickType_t)((configTICK_RATE_HZ + (APP_MOTOR_CTRL_FREQ_HZ / 2)) / APP_MOTOR_CTRL_FREQ_HZ);

    vTaskDelay(pdMS_TO_TICKS(1000));
    if (motor_hw_init() != ESP_OK) return;

    g_motor_ctx.cmd_queue = xQueueCreate(APP_MOTOR_QUEUE_SIZE, sizeof(app_motor_cmd_t));
    if (!g_motor_ctx.cmd_queue) return;

    while (1) {
        /* ---- 顶层待机：复位所有状态，IR 确保关闭 ---- */
        g_motor_ctx.is_active = false;
        g_motor_ctx.state     = MOTOR_STATE_STANDBY;
        motor_hw_stop();
        motor_ir_off();

        if (xQueueReceive(g_motor_ctx.cmd_queue, &cmd, portMAX_DELAY) != pdPASS)
            continue;
        if (cmd.type == APP_MOTOR_CMD_RESUME) continue;
        if (g_motor_ctx.state == MOTOR_STATE_ESTOP) continue;

        g_motor_ctx.is_active = true;
        g_motor_ctx.state     = MOTOR_STATE_ACTIVE;
        /* IR 由 motor_process_cmd 根据命令类型决定开/关，此处不再统一开启 */

        if (!motor_process_cmd(&cmd)) continue;

        TickType_t last_wake_time = xTaskGetTickCount();
        /* 首次 tick 预置一个标称周期，避免 dt=0 导致首拍不动 */
        int64_t    last_tick_us = motor_get_time_us() - (1000000LL / APP_MOTOR_CTRL_FREQ_HZ);
#if APP_MOTOR_DIAG
        g_motor_ctx.diag.win_start_us = last_tick_us;
        g_motor_ctx.diag.dt_max_ms    = 0.0f;
#endif

        while (g_motor_ctx.state == MOTOR_STATE_ACTIVE) {
            int64_t now_us = motor_get_time_us();
            float   dt_s   = (float)(now_us - last_tick_us) / 1000000.0f;
            last_tick_us   = now_us;
            if (dt_s < 0.0f) dt_s = 0.0f;
            if (dt_s > 0.1f) dt_s = 0.1f;  /* 长时间阻塞后限速，避免一步跳满 */

#if APP_MOTOR_DIAG
            motor_diag_tick(now_us, dt_s);
#endif

            switch (g_motor_ctx.exec_mode) {
                case MOTOR_EXEC_REMOTE:
                    motor_exec_remote_tick(dt_s);
                    break;
                case MOTOR_EXEC_AUTO:
                    if (motor_exec_auto_tick(dt_s)) {
                        /* AUTO 完成：切回 REMOTE 模式，IR 关闭 */
                        g_motor_ctx.remote_target_l     = g_motor_ctx.current_speed_l;
                        g_motor_ctx.remote_target_r     = g_motor_ctx.current_speed_r;
                        g_motor_ctx.last_remote_time_us = motor_get_time_us();
                        g_motor_ctx.exec_mode = MOTOR_EXEC_REMOTE;
                        motor_ir_off();
                    }
                    break;
#if ENABLE_MOTOR_IMU_CONTROL
                case MOTOR_EXEC_TURN_ANGLE:
                    if (motor_exec_turn_angle_tick()) {
                        /* 转角完成：切回 IDLE 模式，IR 关闭 */
                        motor_cleanup_previous_mode();
                        g_motor_ctx.remote_target_l = 0;
                        g_motor_ctx.remote_target_r = 0;
                        g_motor_ctx.exec_mode = MOTOR_EXEC_IDLE;
                        motor_ir_off();
                    }
                    break;
#endif
                default:
                    break;
            }

            /* 一次性把队列取空：遥控指令下发频率可能远高于控制频率，
               若每周期只取 1 条，队列会积压，机器人执行的是最多 8 拍之前的旧指令 */
            app_motor_cmd_t new_cmd;
            bool exit_loop = false;
            while (xQueueReceive(g_motor_ctx.cmd_queue, &new_cmd, 0) == pdPASS) {
                if (!motor_process_cmd(&new_cmd)) { exit_loop = true; break; }
            }
            if (exit_loop) break;

            bool should_idle =
                ((g_motor_ctx.exec_mode == MOTOR_EXEC_REMOTE) ||
                 (g_motor_ctx.exec_mode == MOTOR_EXEC_IDLE)) &&
                (fabsf(g_motor_ctx.current_speed_l) < 0.5f) &&
                (fabsf(g_motor_ctx.current_speed_r) < 0.5f) &&
                (fabsf(g_motor_ctx.remote_target_l) < 0.5f) &&
                (fabsf(g_motor_ctx.remote_target_r) < 0.5f);

            if (should_idle) {
                motor_ir_off();
                if (xQueueReceive(g_motor_ctx.cmd_queue, &new_cmd,
                                  pdMS_TO_TICKS(APP_MOTOR_IDLE_TIMEOUT_MS)) == pdPASS) {
                    /* IR 由 motor_process_cmd 按新命令类型决定，此处不再统一开启 */
                    if (!motor_process_cmd(&new_cmd)) break;
                    /* 从长阻塞中唤醒：重置时基，否则 vTaskDelayUntil 会连续空转补帧，
                       白占 CPU 还会让其它核心 0 任务(如 ui_logic)被饿死 */
                    last_wake_time = xTaskGetTickCount();
                    last_tick_us   = motor_get_time_us();
                } else {
                    break;
                }
            }
            vTaskDelayUntil(&last_wake_time, ctrl_period);
        }
    }
}

/**
 * @brief 指令入队
 * @param drop_oldest 队列满时是否丢弃最旧的一条（遥控用：永远以最新意图为准）
 */
static esp_err_t motor_push_cmd(const app_motor_cmd_t* cmd, bool drop_oldest,
                                TickType_t wait_ticks) {
    QueueHandle_t q = g_motor_ctx.cmd_queue;
    if (!q) return ESP_ERR_INVALID_STATE;  /* 电机任务尚未创建队列 */
    if (xQueueSend(q, cmd, wait_ticks) == pdPASS) return ESP_OK;
    if (!drop_oldest) return ESP_ERR_TIMEOUT;
    app_motor_cmd_t dropped;
    if (xQueueReceive(q, &dropped, 0) != pdPASS) return ESP_ERR_TIMEOUT;
    return (xQueueSend(q, cmd, 0) == pdPASS) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t app_motor_init(void) {
    bool dummy1, dummy2;
    dooi_nvs_get_hw_invert(&g_motor_ctx.inv_l, &g_motor_ctx.inv_r, &dummy1, &dummy2);

    g_motor_ctx.ramp_accel   = APP_MOTOR_RAMP_ACCEL;
    g_motor_ctx.ramp_decel   = APP_MOTOR_RAMP_DECEL;
    g_motor_ctx.ramp_reverse = APP_MOTOR_RAMP_REVERSE;

    if (xTaskCreatePinnedToCore(motor_task, "motor_task", APP_MOTOR_TASK_STACK, NULL,
                                APP_MOTOR_TASK_PRIO, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t app_motor_set_remote(float throttle, float steering) {
    app_motor_cmd_t cmd = {
        .type = APP_MOTOR_CMD_REMOTE,
        .params.remote = {throttle, steering}
    };
    /* 队列满时只丢最旧的一条，保证 "最新指令优先"；
       不要用 xQueueReset 清空，那会把队列里的急停等指令一起抹掉 */
    motor_push_cmd(&cmd, true, 0);
    return ESP_OK;
}

esp_err_t app_motor_move_auto(int16_t speed_l, int16_t speed_r, uint32_t duration_ms) {
    app_motor_cmd_t cmd = {
        .type = APP_MOTOR_CMD_AUTO,
        .params.auto_move = {speed_l, speed_r, duration_ms}
    };
    return motor_push_cmd(&cmd, false, pdMS_TO_TICKS(10));
}

#if ENABLE_MOTOR_IMU_CONTROL
esp_err_t app_motor_turn_angle(float target_angle_deg, int16_t max_speed) {
    app_motor_cmd_t cmd = {
        .type = APP_MOTOR_CMD_TURN_ANGLE,
        .params.turn_angle = {.target_angle = target_angle_deg, .max_speed = max_speed}
    };
    return motor_push_cmd(&cmd, false, pdMS_TO_TICKS(10));
}
#endif

esp_err_t app_motor_estop(void) {
    if (!g_motor_ctx.cmd_queue) return ESP_ERR_INVALID_STATE;
    app_motor_cmd_t cmd = {.type = APP_MOTOR_CMD_ESTOP};
    xQueueReset(g_motor_ctx.cmd_queue);  /* 急停：清掉所有待执行指令 */
    xQueueSend(g_motor_ctx.cmd_queue, &cmd, 0);
    return ESP_OK;
}

esp_err_t app_motor_resume(void) {
    app_motor_cmd_t cmd = {.type = APP_MOTOR_CMD_RESUME};
    return motor_push_cmd(&cmd, false, pdMS_TO_TICKS(10));
}

esp_err_t app_motor_stop(void) {
    app_motor_cmd_t cmd = {.type = APP_MOTOR_CMD_STOP};
    return motor_push_cmd(&cmd, false, pdMS_TO_TICKS(10));
}

bool app_motor_is_active(void) { return g_motor_ctx.is_active; }

void app_motor_set_invert(bool inv_l, bool inv_r) {
    g_motor_ctx.inv_l = inv_l;
    g_motor_ctx.inv_r = inv_r;
}

void app_motor_set_ramp(float accel, float decel, float reverse) {
    if (accel   > 0.0f) g_motor_ctx.ramp_accel   = accel;
    if (decel   > 0.0f) g_motor_ctx.ramp_decel   = decel;
    if (reverse > 0.0f) g_motor_ctx.ramp_reverse = reverse;
    ESP_LOGI(TAG, "ramp: accel=%.0f decel=%.0f reverse=%.0f (units/s)",
             g_motor_ctx.ramp_accel, g_motor_ctx.ramp_decel, g_motor_ctx.ramp_reverse);
}