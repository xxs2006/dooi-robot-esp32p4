#include "xbox_handle.h"

#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_periphral.h"
#include "app_behavior.h"
#include "app_servo.h"
#include "app_ui.h"
#include "app_ui_logic.h"
#include "eyes_control.h"
#include "dooi_nvs.h"

static const char *TAG = "XBOX_HDL";

/* ================================================================
 *  常量
 * ================================================================ */

/* ----------------------------------------------------------------
 *  眼睛移动平滑参数（可调）
 * ---------------------------------------------------------------- */

/** 眼睛位置更新最小间隔 (ms)，约 30 Hz
 *  与显示刷新率对齐，超过无意义且会造成撕裂 */
#define EYE_CMD_MIN_INTERVAL_MS     40u

/** 指数低通滤波时间常数 (ms)
 *  τ=60 → α≈0.42 → 响应快但运动顺滑
 *  增大 τ 更丝滑，减小 τ 更跟手         */
#define EYE_TAU_MS                  60.0f

/** 位置死区：双轴变化量均小于此值时跳过刷新
 *  防止摇杆抖动在同一像素内反复触发重绘   */
#define EYE_DEADBAND                0.015f

#define JOY_DEAD_F                  0.05f

/* 扳机滞回阈值（防止在零点附近反复激活/释放） */
#define SERVO_TRIG_PRESS_TH         0.03f
#define SERVO_TRIG_RELEASE_TH       0.01f

/* 扳机最大偏转幅度 (度) */
#define SERVO_MAX_EXCURSION_DEG     90.0f

/* 软件滤波后发送死区：避免 LEDC 被细小数值抖动频繁刷新 */
#define SERVO_ANGLE_DEADBAND        0.4f

/* 最大更新速率门控（约 50 Hz）
 * 同时也是低通滤波的固定步长 dt                          */
#define SERVO_CMD_MIN_INTERVAL_MS   20u

/* 起始位选项表（LB / RB 每按一次循环切换） */
static const float s_servo_home_opts[] = { 180.0f, 135.0f, 90.0f };
#define SERVO_HOME_OPT_COUNT  3

/* ================================================================
 *  A/B/X/Y 情绪序列池
 *
 *  每个池对应一种情绪主题，按键按下时从池中随机抽取一条序列播放。
 *  增删条目即可调整覆盖范围，无需改动任何逻辑代码。
 *  若需提高某条序列的出现概率，重复填写该 ID 即可（加权随机）。
 * ================================================================ */

/** A（绿）→ 快乐 / 积极 */
static const char *const s_seq_a[] = {
    "happy",
    "happy",
    "happy",
    "love",
    "shy",
};

/** B（红）→ 愤怒 / 强烈 */
static const char *const s_seq_b[] = {
    "angry",
    "angry",
    "angry",
    "angry",
    "disdain",
};

/** X（蓝）→ 悲伤 / 消极 */
static const char *const s_seq_x[] = {
    "sad",
    "bored",
    "fear",
    "confused",
};

/** Y（黄）→ 思考 / 好奇 */
static const char *const s_seq_y[] = {
    "thinking",
    "curious",
    "surprised",
    "action_stretch",
    "encourage",
};

/** 安全计算池长度 */
#define SEQ_POOL_SIZE(pool)  ((int)(sizeof(pool) / sizeof((pool)[0])))

/* ================================================================
 *  可导航页面表
 * ================================================================ */

static const screen_type_t s_nav_screens[] = {
    SCREEN_DIGITAL_CLOCK,
    SCREEN_MUSIC_SPECTRUM,
    SCREEN_EYES,            /* ← NAV_HOME_IDX = 2 */
    SCREEN_CAMERA,
    SCREEN_LUCKY_CAT,
    SCREEN_INFO,
};
#define NAV_COUNT    ((int)(sizeof(s_nav_screens) / sizeof(s_nav_screens[0])))
#define NAV_HOME_IDX 2

/* ================================================================
 *  SCREEN_CAMERA 子功能列表
 * ================================================================ */

static const camera_mode_t s_cam_modes[] = {
    CAMERA_MODE_PREVIEW,
    CAMERA_MODE_FACE_DETECT,
    CAMERA_MODE_GESTURE_RECOGNIZE,
};
#define CAM_MODE_COUNT ((int)(sizeof(s_cam_modes) / sizeof(s_cam_modes[0])))

/* ================================================================
 *  内部类型
 * ================================================================ */

/**
 * 单轴舵机运行状态
 *
 * smooth_angle : 经软件指数低通滤波后的命令角度，是实际下发给硬件的值。
 *                它并不代表舵机的物理位置，而是"期望命令"的平滑版本。
 * sent_angle   : 上次真正写入 LEDC 的角度，用于死区比较以避免冗余写入。
 * last_tick    : 上次执行的系统滴答，用于时间门控（限速 ~50 Hz）。
 * home_idx     : 当前起始位在 s_servo_home_opts[] 中的索引。
 * active       : 扳机是否处于激活区间（带滞回防抖）。
 */
typedef struct {
    bool       active;
    int        home_idx;
    float      smooth_angle;
    float      sent_angle;
    TickType_t last_tick;
} servo_axis_t;

/**
 * 手柄全局运行状态
 *
 * iris_idx : 当前虹膜风格在 EYES_IRIS_DEFINE 枚举中的索引，
 *            随全局复位一起归 EYES_IRIS_DEFAULT。
 */
typedef struct {
    xbox_buttons_t prev_btns;
    int nav_idx;
    int clock_mode;
    int emotion_idx;
    int cam_mode_idx;
    int cat_style;
    int iris_idx;
} xbox_hdl_t;

/**
 * 眼睛位置运行状态（对标 servo_axis_t 设计）
 *
 * smooth_x/y : 指数低通滤波后的命令坐标
 * sent_x/y   : 上次真正写入显示的坐标（死区比较用）
 * last_tick  : 上次执行的系统滴答（时间门控用）
 */
typedef struct {
    float      smooth_x;
    float      smooth_y;
    float      sent_x;
    float      sent_y;
    TickType_t last_tick;
} eye_state_t;

static eye_state_t s_eye = {
    .smooth_x  = 0.0f,
    .smooth_y  = 0.0f,
    .sent_x    = 0.0f,
    .sent_y    = 0.0f,
    .last_tick = 0,
};

/* ================================================================
 *  私有变量
 * ================================================================ */

static xbox_hdl_t s_h = {
    .nav_idx      = NAV_HOME_IDX,
    .clock_mode   = (int)MODE_TIME,
    .emotion_idx  = (int)EYES_EMOTION_DEFAULT,
    .cam_mode_idx = 0,
    .cat_style    = 1,
    .iris_idx     = (int)EYES_IRIS_DEFAULT,
};

/* 两轴均初始化在 180°（home_idx=0 对应 s_servo_home_opts[0]=180°） */
static servo_axis_t s_srv[2] = {
    { .active=false, .home_idx=0, .smooth_angle=180.0f, .sent_angle=180.0f, .last_tick=0 },
    { .active=false, .home_idx=0, .smooth_angle=180.0f, .sent_angle=180.0f, .last_tick=0 },
};

/* ================================================================
 *  辅助
 * ================================================================ */

static float joy_norm(uint16_t raw, bool invert)
{
    float v = ((float)raw - (float)XBOX_JOY_CENTER) / (float)XBOX_JOY_CENTER;
    if (fabsf(v) < JOY_DEAD_F) return 0.0f;
    if (v >  1.0f) v =  1.0f;
    if (v < -1.0f) v = -1.0f;
    return invert ? -v : v;
}

static inline int cycle(int val, int dir, int mod)
{
    return ((val + dir) % mod + mod) % mod;
}

/* ================================================================
 *  持续帧驱动
 * ================================================================ */
static void do_motor(const xbox_controller_state_t *x)
{
    static float last_v = 0.0f;
    static float last_h = 0.0f;
    
    float v = joy_norm(x->analog.joy_lv, true);
    float h = joy_norm(x->analog.joy_lh, true);
    
    // 本次和上次的值都为 0，无需重复发送
    if (v == 0.0f && h == 0.0f && last_v == 0.0f && last_h == 0.0f) {
        return;
    }
    
    app_motor_set_remote(v, h);
    last_v = v;
    last_h = h;
}
static void do_eye(const xbox_controller_state_t *x)
{
    /* ── 1. 时间门控：限制最大刷新率 ~30 Hz ── */
    TickType_t now = xTaskGetTickCount();
    if (s_eye.last_tick != 0 &&
        (now - s_eye.last_tick) < pdMS_TO_TICKS(EYE_CMD_MIN_INTERVAL_MS)) {
        return;
    }
    s_eye.last_tick = now;

    /* ── 2. 目标位置：摇杆在死区内自动归零 ── */
    float tx = xbox_joy_right_active(x) ? joy_norm(x->analog.joy_rh, false) : 0.0f;
    float ty = xbox_joy_right_active(x) ? joy_norm(x->analog.joy_rv, false) : 0.0f;

    /* ── 3. 指数低通滤波
     *  α = 1 - exp(-dt / τ)，dt 固定 = EYE_CMD_MIN_INTERVAL_MS
     *  归中与跟随共用同一 τ，保持顺滑一致性               ── */
    const float alpha = 1.0f - expf(-(float)EYE_CMD_MIN_INTERVAL_MS / EYE_TAU_MS);
    s_eye.smooth_x += (tx - s_eye.smooth_x) * alpha;
    s_eye.smooth_y += (ty - s_eye.smooth_y) * alpha;

    /* ── 4. 死区过滤：双轴均无明显变化则跳过渲染 ── */
    float dx = fabsf(s_eye.smooth_x - s_eye.sent_x);
    float dy = fabsf(s_eye.smooth_y - s_eye.sent_y);
    if (dx < EYE_DEADBAND && dy < EYE_DEADBAND) {
        return;
    }

    eye_controller_set_position(s_eye.smooth_x, s_eye.smooth_y);
    s_eye.sent_x = s_eye.smooth_x;
    s_eye.sent_y = s_eye.smooth_y;
}

/**
 * @brief 单轴舵机油门控制（指数低通滤波版）
 *
 * 流程：
 *   1. 时间门控  ── 限制最大刷新率，防止 LEDC 指令堆积
 *   2. 滞回判断  ── 确定 active 状态，消除零点附近的颤动
 *   3. 目标计算  ── active: home - ratio×90°  |  inactive: home
 *   4. 指数平滑  ── smooth += (target - smooth) × α
 *                   α = 1 - exp(−dt/τ)，固定步长 dt = SERVO_CMD_MIN_INTERVAL_MS
 *                   active 时 τ=SERVO_TAU_ACTIVE_MS，松开时 τ=SERVO_TAU_RETURN_MS
 *   5. 死区过滤  ── |smooth - sent| >= SERVO_ANGLE_DEADBAND 才写 LEDC
 *
 * 发送时 duration_ms=0（直接写 LEDC 占空比），
 * 平滑完全由第 4 步的软件滤波负责，无需依赖硬件渐变。
 *
 * @param idx   轴索引 (0=左, 1=右)
 * @param id    硬件枚举 servo_id_t
 * @param ratio 扳机深度 [0.0, 1.0]
 */
static void do_servo_axis(int idx, servo_id_t id, float ratio)
{
    servo_axis_t *s    = &s_srv[idx];
    float         home = s_servo_home_opts[s->home_idx];

    /* ── 1. 时间门控 ── */
    TickType_t now = xTaskGetTickCount();
    if (s->last_tick != 0 &&
        (now - s->last_tick) < pdMS_TO_TICKS(SERVO_CMD_MIN_INTERVAL_MS)) {
        return;
    }
    s->last_tick = now;

    /* ── 2. 滞回激活判断 ── */
    if (!s->active) {
        if (ratio >= SERVO_TRIG_PRESS_TH)   s->active = true;
    } else {
        if (ratio <  SERVO_TRIG_RELEASE_TH) s->active = false;
    }

    /* ── 3. 目标角度 & 时间常数 ── */
    float target, tau;
    if (s->active) {
        target = home - ratio * SERVO_MAX_EXCURSION_DEG;
        if (target < 0.0f) target = 0.0f;
        tau = SERVO_TAU_ACTIVE_MS;
    } else {
        target = home;
        tau    = SERVO_TAU_RETURN_MS;
    }

    /* ── 4. 指数低通滤波
     *  固定步长 dt = SERVO_CMD_MIN_INTERVAL_MS，因为时间门控保证每次间隔约等于 dt。
     *  首次调用（last_tick 刚被赋值前为 0）也用相同 dt，smooth_angle 从 home 出发
     *  向 target 迈出第一小步，行为正确。                                          ── */
    const float alpha = 1.0f - expf(-(float)SERVO_CMD_MIN_INTERVAL_MS / tau);
    s->smooth_angle  += (target - s->smooth_angle) * alpha;

    /* ── 5. 死区过滤后发送 ── */
    if (fabsf(s->smooth_angle - s->sent_angle) >= SERVO_ANGLE_DEADBAND) {
        app_servo_set_angle(id, s->smooth_angle, 0);
        s->sent_angle = s->smooth_angle;
    }
}

static void do_servo(const xbox_controller_state_t *x)
{
    float lt = (float)x->analog.trig_lt / (float)XBOX_TRIG_MAX;
    float rt = (float)x->analog.trig_rt / (float)XBOX_TRIG_MAX;
    if (lt > 1.0f) lt = 1.0f;
    if (rt > 1.0f) rt = 1.0f;

    do_servo_axis(0, SERVO_LEFT,  lt);
    do_servo_axis(1, SERVO_RIGHT, rt);
}

/* ================================================================
 *  按键边缘触发
 * ================================================================ */

static void do_take_photo(void)
{
    ESP_LOGI(TAG, "Share → take photo");
    app_event_t evt = {.type = APP_EVT_CAMERA_TIGGRER_PREVIEW};
    app_ui_logic_post(&evt, false);
}

/**
 * @brief LB/RB：循环切换舵机起始位 180° → 135° → 90° → 180° …
 *
 * 只需更新 home_idx；do_servo_axis 的归位 lerp 会在后续帧中
 * 以 SERVO_TAU_RETURN_MS 自然过渡到新起始位，无需额外发送硬件指令。
 */
static void do_servo_home_cycle(int idx)
{
    servo_axis_t *s = &s_srv[idx];
    s->home_idx = (s->home_idx + 1) % SERVO_HOME_OPT_COUNT;
    ESP_LOGI(TAG, "Servo[%d] 起始位 → %.0f°", idx, s_servo_home_opts[s->home_idx]);
}

/**
 * @brief A/B/X/Y：从情绪场景池中随机抽取一个播放
 *
 * 使用 ESP 硬件随机数源（esp_random），确保每次按键体验不重复。
 * 附带短震动反馈（力度 25，持续 30 ms）增强按键触感；
 * 如不需要震动，删除 xbox_vibration_pulse 那一行即可。
 *
 * @param pool  场景名称数组首地址
 * @param count 数组长度
 */
static void do_emotion_random(const char *const *pool, int count)
{
    if (count <= 0) return;

    int idx = (int)(esp_random() % (uint32_t)count);

    ESP_LOGI(TAG, "emotion → scene %s (pool[%d])", pool[idx], idx);
    app_ui_logic_request_behavior(pool[idx], NULL, BEHAVIOR_PRIO_DEFAULT);

    /* 短震动反馈：力度 25 / 持续约 30 ms（3 × 10 ms） */
    xbox_vibration_pulse(25, 3);
}

/**
 * @brief START：循环切换虹膜风格
 *
 * 顺序：DEFAULT → EMERALD → VIOLET → BLUE → AMBER
 *       → GRAY → RUBY → WINERED → DEFAULT → …
 */
static void do_iris_cycle(void)
{
    /* 首次使用时把本地索引与持久化值对齐，避免“显示的是 A、循环却从 B 开始” */
    static bool s_iris_synced = false;
    if (!s_iris_synced) {
        s_iris_synced = true;
        uint8_t stored = (uint8_t)s_h.iris_idx;
        if (dooi_nvs_get_eye_iris(&stored) == ESP_OK) {
            s_h.iris_idx = (int)stored % (int)EYES_IRIS_COUNT;
        }
    }

    s_h.iris_idx = (s_h.iris_idx + 1) % (int)EYES_IRIS_COUNT;
    /* 统一走 dooi_nvs 落盘（内部同时应用虹膜+情绪），重启后保持用户选择 */
    dooi_nvs_set_eye_iris((uint8_t)s_h.iris_idx);

    static const char *const s_iris_names[] = {
        "DEFAULT", "EMERALD", "VIOLET", "BLUE",
        "AMBER",   "GRAY",    "RUBY",   "WINERED",
    };
    ESP_LOGI(TAG, "START → iris → %s (%d)",
             s_iris_names[s_h.iris_idx], s_h.iris_idx);
}

/**
 * @brief SELECT：快捷开启人脸追踪循环模式
 *
 * 触发后摄像头切换至 FACE_DETECT 模式并持续循环跟踪，
 * 直到被其他摄像头指令覆盖。
 */
static void do_face_track(void)
{
    ESP_LOGI(TAG, "SELECT → face track loop");
    camera_request_t req = {
        .mode    = CAMERA_MODE_FACE_DETECT,
        .trigger = CAMERA_TRIGGER_FACE_TRACK_LOOP,
    };
    camera_trigger_funtion(&req);
}

static void do_reset(void)
{
    ESP_LOGI(TAG, "XBOX → reset → SCREEN_EYES");

    // app_motor_set_remote(0.0f, 0.0f);
    app_led_off();
    // eye_controller_set_position(0.0f, 0.0f);

    /* 眼睛软件状态清零 */
    // s_eye.smooth_x  = s_eye.smooth_y = 0.0f;
    // s_eye.sent_x    = s_eye.sent_y   = 0.0f;
    // s_eye.last_tick = 0;

    /* 虹膜归默认 */
    // s_h.iris_idx = (int)EYES_IRIS_DEFAULT;
    // eye_controller_apply_iris_texture(EYES_IRIS_DEFAULT);

    /* 双舵机硬件渐变归 180°（绕过软件 lerp，立即发出硬件渐变指令） */
    app_servo_set_target_angle(SERVO_TARGET_BOTH, 135.0f, SERVO_RESET_MS);

    /* 同步软件状态，确保复位后 lerp 从正确位置出发 */
    for (int i = 0; i < 2; i++) {
        s_srv[i].active       = false;
        s_srv[i].home_idx     = 0;        /* 索引 0 = 135° */
        s_srv[i].smooth_angle = 135.0f;
        s_srv[i].sent_angle   = 135.0f;
        s_srv[i].last_tick    = 0;
    }

    // s_h.nav_idx      = NAV_HOME_IDX;
    // s_h.clock_mode   = (int)MODE_TIME;
    // s_h.emotion_idx  = (int)EYES_EMOTION_DEFAULT;
    // s_h.cam_mode_idx = 0;
    // s_h.cat_style    = 1;
    /* iris_idx 已在上方归位，无需重复 */

    app_ui_logic_request_screen(SCREEN_EYES);
}

static void do_page(bool up)
{
    s_h.nav_idx = up
        ? (s_h.nav_idx - 1 + NAV_COUNT) % NAV_COUNT
        : (s_h.nav_idx + 1) % NAV_COUNT;
    screen_type_t scr = s_nav_screens[s_h.nav_idx];
    ESP_LOGI(TAG, "page → screen=%d", (int)scr);
    app_ui_logic_request_screen(scr);
}

static void do_ui_lr(int dir)
{
    switch (screen_get_current()) {

        case SCREEN_DIGITAL_CLOCK:
            s_h.clock_mode = cycle(s_h.clock_mode, dir, 2);
            digital_clock_mode_set(s_h.clock_mode);
            ESP_LOGI(TAG, "clock mode → %s",
                     s_h.clock_mode == (int)MODE_TIME ? "TIME" : "TIMER");
            break;

        case SCREEN_EYES:
            s_h.emotion_idx = cycle(s_h.emotion_idx, dir, (int)EYES_EMOTION_COUNT);
            eye_controller_apply_emotion((EYES_EMOTION)s_h.emotion_idx);
            ESP_LOGI(TAG, "eye emotion → %d", s_h.emotion_idx);
            break;

        case SCREEN_CAMERA:
            s_h.cam_mode_idx = cycle(s_h.cam_mode_idx, dir, CAM_MODE_COUNT);
            camera_mode_switch(s_cam_modes[s_h.cam_mode_idx]);
            ESP_LOGI(TAG, "camera mode → %d", (int)s_cam_modes[s_h.cam_mode_idx]);
            break;

        case SCREEN_LUCKY_CAT:
            s_h.cat_style = cycle(s_h.cat_style - 1, dir, 4) + 1;
            lucky_cat_style_set(s_h.cat_style);
            ESP_LOGI(TAG, "lucky cat style → %d", s_h.cat_style);
            break;

        default:
            break;
    }
}

static void do_buttons(const xbox_controller_state_t *x)
{
    const xbox_buttons_t *c = &x->buttons;
    const xbox_buttons_t *p = &s_h.prev_btns;

#define RISE(f) (c->f && !p->f)

    /* ── 拍照 ── */
    if (RISE(btn_share)) do_take_photo();

    /* ── 全局复位（优先处理，复位后直接返回） ── */
    if (RISE(btn_xbox)) {
        do_reset();
        s_h.prev_btns = *c;
        return;
    }

    /* ── 舵机起始位循环切换 ── */
    if (RISE(btn_lb)) do_servo_home_cycle(0);
    if (RISE(btn_rb)) do_servo_home_cycle(1);

    /* ── 情绪序列（随机抽取播放）────────────────────────────────
     *   A（绿）→ 快乐/积极    B（红）→ 愤怒/强烈
     *   X（蓝）→ 悲伤/消极    Y（黄）→ 思考/好奇
     * ─────────────────────────────────────────────────────────── */
    if (RISE(btn_a)) do_emotion_random(s_seq_a, SEQ_POOL_SIZE(s_seq_a));
    if (RISE(btn_b)) do_emotion_random(s_seq_b, SEQ_POOL_SIZE(s_seq_b));
    if (RISE(btn_x)) do_emotion_random(s_seq_x, SEQ_POOL_SIZE(s_seq_x));
    if (RISE(btn_y)) do_emotion_random(s_seq_y, SEQ_POOL_SIZE(s_seq_y));

    /* ── 虹膜风格循环切换 ── */
    if (RISE(btn_start))  do_iris_cycle();

    /* ── 人脸追踪快捷开启 ── */
    if (RISE(btn_select)) do_face_track();

    /* ── 页面导航 ── */
    if (RISE(btn_dir_up))    do_page(true);
    if (RISE(btn_dir_down))  do_page(false);
    if (RISE(btn_dir_left))  do_ui_lr(-1);
    if (RISE(btn_dir_right)) do_ui_lr(+1);

#undef RISE

    s_h.prev_btns = *c;
}

/* ================================================================
 *  主入口
 * ================================================================ */

void xbox_handle_process(xbox_controller_state_t *xbox)
{
    if (!xbox || !xbox->connected) return;

    do_motor(xbox);
    do_eye(xbox);
    do_servo(xbox);
    do_buttons(xbox);
}