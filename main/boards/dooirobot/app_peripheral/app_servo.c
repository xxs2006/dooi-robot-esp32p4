#include "app_servo.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <math.h>
#include <string.h>
#include "dooi_nvs.h"

static const char *TAG = "SERVO";

/* ================= 配置参数 ================= */
#define SERVO_LEDC_MODE         LEDC_LOW_SPEED_MODE
#define SERVO_LEDC_TIMER        LEDC_TIMER_1
#define SERVO_LEDC_RES          LEDC_TIMER_13_BIT // 13位分辨率 (0-8191)
#define SERVO_FREQ_HZ           50                // 50Hz (周期20ms)

// 脉宽设置 (单位us) - 对应 SG90/MG90S 等标准舵机
#define SERVO_MIN_PULSE_US      500
#define SERVO_MAX_PULSE_US      2500
#define SERVO_MAX_DEGREE        180.0f

// 校准偏移量程 (±度)
#define SERVO_TRIM_MAX_DEG      30.0f

// 任务配置
#define SERVO_TASK_STACK        1536
#define SERVO_TASK_PRIO         10
#define SERVO_QUEUE_LEN         5    
#define SERVO_IDLE_TIMEOUT_MS   1000  

/* ================= 内部结构 ================= */

// 硬件映射表
static const struct {
    int pin;
    ledc_channel_t channel;
} servo_hw_map[SERVO_MAX_COUNT] = {
    [SERVO_LEFT]  = { .pin = SERVO_PIN_LEFT,  .channel = LEDC_CHANNEL_4 },
    [SERVO_RIGHT] = { .pin = SERVO_PIN_RIGHT, .channel = LEDC_CHANNEL_5 },
};

// 任务指令
typedef struct {
    bool is_sync;           // 是否同步指令
    servo_id_t target_id;   // 单控ID
    float angles[SERVO_MAX_COUNT];        // [0]:Left/Target, [1]:Right
    
    uint32_t duration_ms;   // 耗时
} servo_cmd_t;

// 运行时上下文
typedef struct {
    float trim[SERVO_MAX_COUNT];      // 微调值
    uint32_t current_duty[SERVO_MAX_COUNT]; // 当前占空比缓存
    float current_angle[SERVO_MAX_COUNT];
    bool power_on;
    QueueHandle_t queue;

    bool inv_l;
    bool inv_r;
} servo_ctx_t;

static servo_ctx_t g_servo;

/* ================= 辅助函数 ================= */

/**
 * @brief 将“硬件输出角”(已含镜像/反向/校准)转换为 LEDC 占空比数值
 * 优化：使用整型运算尽量减少浮点开销，预编译常量
 */
static uint32_t angle_to_duty(float angle)
{
    // 边界限制
    if (angle < 0.0f) angle = 0.0f;
    else if (angle > SERVO_MAX_DEGREE) angle = SERVO_MAX_DEGREE;

    // 计算脉宽 (us)
    // pulse = MIN + (angle / MAX_DEG) * (MAX - MIN)
    uint32_t pulse_us = SERVO_MIN_PULSE_US + 
                        (uint32_t)((angle / SERVO_MAX_DEGREE) * (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US));

    // 计算占空比数值
    // Duty = (Pulse_us / 20000us) * (2^13 - 1)
    // 2^13 - 1 = 8191
    return (pulse_us * 8191) / 20000;
}

// 硬件电源控制
static void hw_power_ctrl(bool enable)
{
    if (g_servo.power_on == enable) return;
    
    gpio_set_level(SERVO_PIN_PWR_CTRL, enable ? 1 : 0);
    g_servo.power_on = enable;

    if (enable) {
        // 上电后短暂延时等待电压稳定，避免单片机复位
        vTaskDelay(pdMS_TO_TICKS(20)); 
    }
}

// 执行硬件动作
static void execute_move(servo_id_t id, float angle, uint32_t duration_ms)
{
    /*
     * 校准顺序（关键）：
     *   1) 先在“逻辑角度”空间叠加 trim。左右两侧使用同一符号语义，
     *      右侧舵机镜像安装时，其物理转向与左侧相反，
     *      因此在逻辑空间统一叠加后，经下方镜像换算会自动对右侧取反，
     *      即硬件实际偏移 = -trim_r（左右校准值不再需要手工反号）。
     *   2) 再做机械镜像（右舵机 180° 镜像）。
     *   3) 最后应用反向开关。
     */
    float logical = angle + g_servo.trim[id];
    if (logical < 0.0f) logical = 0.0f;
    else if (logical > SERVO_MAX_DEGREE) logical = SERVO_MAX_DEGREE;

    g_servo.current_angle[id] = angle;

    bool  mirrored      = (id == SERVO_RIGHT);
    float mapped_angle  = mirrored ? (SERVO_MAX_DEGREE - logical) : logical;

    // 检查反向开关，如果开启，则对镜像后的输出再做 180 度翻转 (0-180 -> 180-0)
    bool is_inverted = mirrored ? g_servo.inv_r : g_servo.inv_l;
    if (is_inverted) {
        mapped_angle = SERVO_MAX_DEGREE - mapped_angle;
    }

    uint32_t target_duty = angle_to_duty(mapped_angle);
    ledc_channel_t ch = servo_hw_map[id].channel;

    // 更新缓存
    g_servo.current_duty[id] = target_duty;

    if (duration_ms == 0) {
        // 快速模式：直接设定
        ledc_set_duty(SERVO_LEDC_MODE, ch, target_duty);
        ledc_update_duty(SERVO_LEDC_MODE, ch);
    } else {
        // 平滑模式：使用硬件渐变 (CPU 无需干预)
        ledc_set_fade_with_time(SERVO_LEDC_MODE, ch, target_duty, duration_ms);
        ledc_fade_start(SERVO_LEDC_MODE, ch, LEDC_FADE_NO_WAIT);
    }
}

/* ================= 核心任务 ================= */
static void servo_task(void *arg)
{
    servo_cmd_t cmd;
    TickType_t  power_on_tick = 0;

    vTaskDelay(pdMS_TO_TICKS(10000));

    /* ── 系统初始化：上电归位 → 断电 ── */
    hw_power_ctrl(true);
    execute_move(SERVO_LEFT,  180.0f, 0);
    execute_move(SERVO_RIGHT, 180.0f, 0);
    vTaskDelay(pdMS_TO_TICKS(2000));
    hw_power_ctrl(false);

    while (1) {
        if (xQueueReceive(g_servo.queue, &cmd, portMAX_DELAY) != pdPASS) continue;

        hw_power_ctrl(true);
        power_on_tick = xTaskGetTickCount();   /* 记录本次上电时刻 */

        while (true) {

            /* ── 执行指令 ── */
            if (cmd.is_sync) {
                execute_move(SERVO_LEFT,  cmd.angles[0], cmd.duration_ms);
                execute_move(SERVO_RIGHT, cmd.angles[1], cmd.duration_ms);
            } else {
                execute_move(cmd.target_id, cmd.angles[0], cmd.duration_ms);
            }

            /* ── 等待动作完成（新指令可打断当前动作）── */
            TickType_t move_wait = (cmd.duration_ms > 0)
                                   ? pdMS_TO_TICKS(cmd.duration_ms) : 1;
            servo_cmd_t next;
            if (xQueueReceive(g_servo.queue, &next, move_wait) == pdPASS) {
                cmd = next;     /* 被新指令打断，立即执行新指令 */
                continue;
            }

            TickType_t elapsed   = xTaskGetTickCount() - power_on_tick;
            TickType_t min_ticks = pdMS_TO_TICKS(SERVO_MIN_ON_MS);
            TickType_t remaining = (elapsed < min_ticks) ? (min_ticks - elapsed) : 0;
            TickType_t idle_t    = pdMS_TO_TICKS(SERVO_IDLE_TIMEOUT_MS);
            TickType_t wait_off  = (remaining > idle_t) ? remaining : idle_t;

            if (xQueueReceive(g_servo.queue, &next, wait_off) == pdPASS) {
                cmd = next;     /* 在等待期间收到新指令，续期 */
                continue;
            }

            /* ── 超时，关电，退出活跃循环，回待机 ── */
            hw_power_ctrl(false);
            break;
        }
    }
}

/* ================= 外部接口实现 ================= */

void app_servo_init(void)
{
    // 1. 配置电源脚
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SERVO_PIN_PWR_CTRL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(SERVO_PIN_PWR_CTRL, 0); // 默认关闭

    // 2. 配置 LEDC 定时器
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = SERVO_LEDC_MODE,
        .timer_num        = SERVO_LEDC_TIMER,
        .duty_resolution  = SERVO_LEDC_RES,
        .freq_hz          = SERVO_FREQ_HZ,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // 3. 配置 LEDC 通道
    for (int i = 0; i < SERVO_MAX_COUNT; i++) {
        ledc_channel_config_t ledc_conf = {
            .speed_mode     = SERVO_LEDC_MODE,
            .channel        = servo_hw_map[i].channel,
            .timer_sel      = SERVO_LEDC_TIMER,
            .intr_type      = LEDC_INTR_DISABLE,
            .gpio_num       = servo_hw_map[i].pin,
            .duty           = 0,
            .hpoint         = 0
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ledc_conf));
    }

    ledc_fade_func_install(0);

    bool dummy1, dummy2;
    dooi_nvs_get_hw_invert(&dummy1, &dummy2, &g_servo.inv_l, &g_servo.inv_r);

    /* 从 NVS 加载 Trim 校准值 */
    app_servo_load_trim_from_nvs();

    // 5. 创建资源
    g_servo.queue = xQueueCreate(SERVO_QUEUE_LEN, sizeof(servo_cmd_t));
    g_servo.power_on = false;

    xTaskCreatePinnedToCore(servo_task, "servo_task", SERVO_TASK_STACK, NULL, SERVO_TASK_PRIO, NULL, 0);
    
    ESP_LOGI(TAG, "Servo Initialized. Hardware Fading Enabled.");
}

void app_servo_set_trim(servo_id_t id, float trim_angle)
{
    if (id >= SERVO_MAX_COUNT) return;

    if (trim_angle > SERVO_TRIM_MAX_DEG)  trim_angle = SERVO_TRIM_MAX_DEG;
    if (trim_angle < -SERVO_TRIM_MAX_DEG) trim_angle = -SERVO_TRIM_MAX_DEG;

    g_servo.trim[id] = trim_angle;
}

void app_servo_set_angle(servo_id_t id, float angle, uint32_t duration_ms)
{
    if (id >= SERVO_MAX_COUNT) return;

    servo_cmd_t cmd = {
        .is_sync = false,
        .target_id = id,
        .angles = { angle, 0 },
        .duration_ms = duration_ms
    };
    if (g_servo.queue) xQueueSend(g_servo.queue, &cmd, 0);
}

void app_servo_move_sync(float angle_left, float angle_right, uint32_t duration_ms)
{
    servo_cmd_t cmd = {
        .is_sync = true,
        .angles = { angle_left, angle_right },
        .duration_ms = duration_ms
    };
    if (g_servo.queue) xQueueSend(g_servo.queue, &cmd, 0);
}

void app_servo_set_target_angle(servo_target_t target, float angle, uint32_t duration_ms)
{
    if(target == SERVO_TARGET_BOTH)
    {
        app_servo_move_sync(angle, angle, duration_ms);
    }
    else
    {
        app_servo_set_angle(target, angle, duration_ms);
    }
}

void app_servo_set_invert(bool inv_l, bool inv_r)
{
    g_servo.inv_l = inv_l;
    g_servo.inv_r = inv_r;
    ESP_LOGI(TAG, "Servo invert set: L=%d R=%d", inv_l, inv_r);
}

float app_servo_get_angle(servo_id_t id)
{
    if (id >= SERVO_MAX_COUNT) return -1.0f;
    return g_servo.current_angle[id];
}

void app_servo_load_trim_from_nvs(void)
{
    float trim_l = 0.0f, trim_r = 0.0f;
    dooi_nvs_get_servo_trim(&trim_l, &trim_r);
    app_servo_set_trim(SERVO_LEFT,  trim_l);
    app_servo_set_trim(SERVO_RIGHT, trim_r);
    ESP_LOGI(TAG, "Servo trim loaded: L=%.1f R=%.1f (HW R=%.1f)", trim_l, trim_r, -trim_r);
}
