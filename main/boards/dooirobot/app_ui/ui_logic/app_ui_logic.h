#ifndef __APP_UI_LOGIC_H__
#define __APP_UI_LOGIC_H__

#include <stdint.h>
#include <stdbool.h>
#include "app_ui.h"
#include "app_periphral.h"
#include "app_vision.h"
#include "logic_timeout.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOGIC_QUEUE_DEPTH     5
#define LOGIC_TASK_PRIORITY   10
#define LOGIC_TASK_STACK      4096

typedef enum {
    APP_EVT_NONE,

    /* ── 对话 / 系统状态事件 ── */
    APP_EVT_STATE_LOADING_PROTOCOL,      // 开机加载协议
    APP_EVT_STATE_CONNECT_WIFI,          // 连接 WiFi
    APP_EVT_STATE_STANDBY,               // 待命
    APP_EVT_STATE_LISTENING,             // 监听
    APP_EVT_STATE_SPEAKING,              // 说话
    APP_EVT_STATE_CONNECTING,            // 连接中
    APP_EVT_STATE_ACTIVATION,            // 激活
    APP_EVT_STATE_CHECKING_NEW_VERSION,  // 检查新版本
    APP_EVT_STATE_ERROR,                 // 错误
    APP_EVT_STATE_IDLE,                  // 待机

    /* ── 系统 / 连接事件 ── */
    APP_EVT_BLUETOOTH_OPEN,
    APP_EVT_BLUETOOTH_CLOSE,
    APP_EVT_WEB_SERVER_CONNECT,
    APP_EVT_WEB_SERVER_DISCONNECT,

    APP_EVT_CAMERA_TIGGRER_PREVIEW,
    APP_EVT_IR_ENABLE,
    APP_EVT_IR_DISABLE,
    APP_EVT_IR_TRIGGER,
    APP_EVT_BUTTON,
    APP_EVT_POWER_BTN_PRESS,
    APP_EVT_POWER_SHUTDOWN,
    APP_EVT_IMU,
    APP_EVT_POWER_STATUS,

    APP_EVT_VISION_TASK_EXITED,         // 视觉任务退出
    APP_EVT_VISION_TASK_STARTED,        // 视觉任务启动
    APP_EVT_FACE_DETECTED,              // 人脸检测到
    APP_EVT_FACE_ALIGNED,               // 人脸对齐完成
    APP_EVT_GESTURE,                    // 手势识别结果

    APP_EVT_COUNT
} app_event_type_t;

typedef union {
    struct {
        ir_sensor_id_t sensor_id;
    } ir_data;

    struct {
        int16_t           btn_id;
        touch_btn_event_t event;
    } btn_data;

    struct {
        imu_tilt_dir_t dir;
    } imu_data;

    struct {
        bool is_charging;
        bool low_battery;
    } power_status;

    struct {
        int reverse;
    } reverse;

    struct {
        screen_type_t old_screen;
    } screen_state;       // UI 线程回报给逻辑层的屏幕状态

    struct {
        vision_result_t result;
    } vision_result;

    struct {
        gesture_type_t current;
        gesture_type_t previous;
        float          score;
    } gesture_data;

    struct {
        vision_mode_t mode;
        vision_intervals_t intervals;
    } vision_task_data;

} app_event_data_t;

typedef struct {
    app_event_type_t  type;
    app_event_data_t  data;
} app_event_t;

typedef struct {
    app_event_type_t current_state;     // 当前对话状态
    app_event_type_t prev_state;        // 上一个对话状态
    bool sys_is_init_done;
    bool is_conv_state;                 // 是否处于对话状态（LISTENING 或 SPEAKING）
} app_system_state_t;

/* 全局系统状态，供各 logic_xxx 子模块共享读写 */
extern app_system_state_t g_app_state;

/**
 * @brief 切换当前对话/系统状态，并记录上一个状态
 */
void set_conv_state(app_event_type_t new_state);

/**
 * @brief 初始化逻辑层，创建事件队列和逻辑任务
 */
void app_ui_logic_init(void);

/**
 * @brief 向逻辑层投递事件（线程安全，可从任意任务/ISR 调用）
 * @param evt      事件指针
 * @param from_isr 是否来自中断上下文
 * @return true 投递成功
 */
bool app_ui_logic_post(const app_event_t *evt, bool from_isr);

/* 便捷封装：请求切换到指定屏幕 */
void app_ui_logic_request_screen(screen_type_t screen);

/* 逻辑层请求渲染层控制待机计时器（trigger=1 触发待机，0 仅重置计时） */
void app_ui_logic_request_idle(int trigger);

/**
 * @brief 请求整机关机（线程安全，可从任意任务调用，例如 MCP / 网络回调）
 *
 * 仅向逻辑层投递 APP_EVT_POWER_SHUTDOWN 事件后立刻返回，真正的优雅关机流程
 * （停对话 → 停行为 → 屏幕提示 → 切断电源）由逻辑任务中的 on_power_shutdown 执行，
 * 因此本函数不会阻塞调用方，调用方仍有机会把响应（如 MCP 结果）发出去。
 *
 * @return true  事件投递成功（仅代表已受理，不代表已断电）
 *         false 投递失败（逻辑层未初始化或队列忙）
 */
bool app_ui_logic_request_shutdown(void);

/**
 * @brief UI 线程调用：将当前/上一个屏幕回报给逻辑层，
 *        使逻辑层无需直接读取渲染层的 screen_manager。
 */
void app_ui_logic_report_screen(screen_type_t current, screen_type_t old);

/**
 * @brief 读取逻辑层自有的当前屏幕（镜像渲染层，避免逻辑层直接访问 screen_manager）
 */
screen_type_t app_ui_logic_get_current_screen(void);

/**
 * @brief 查询当前对话状态
 */
app_event_type_t app_ui_logic_get_state(void);

/* UI 心跳：直接由 UI 线程 25ms 主循环调用，推进超时状态机（WAKEUP_ACK/IDLE）。
 * 不走事件队列，避免每 tick 一次跨核 round-trip。 */
void app_ui_logic_tick(void);

bool app_ui_logic_is_conv_state();

bool app_ui_logic_is_init_done(void);

/* ── 行为网关（定义在 dooi_behavior.c，声明在此以便各模块统一引用）──
 * 所有"让机器人执行某个行为"的请求都应经此入口，而非直接调用 app_behavior_play_scene。 */
bool app_ui_logic_request_behavior(const char *scene_name, const char *variant, int prio);
const char *app_ui_logic_last_behavior_scene(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_UI_LOGIC_H__ */