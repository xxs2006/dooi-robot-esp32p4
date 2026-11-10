#include "pages.h"
#include "app_ui_logic.h"
#include "app_behavior.h"
#include "eyes_config.h"
#include "app_behavior_status.h"

#include "esp_log.h"

static const char *TAG = "logic_page_eyes";

/* ================================================================
 *                     手 势 识 别 配 置
 * ================================================================ */
#define GESTURE_HISTORY_MAX             8
#define GESTURE_RESET_MS                20000
#define GESTURE_COOLDOWN_MS             350

#define WIN_FAST_TAP_SURPRISED_MS       3000
#define WIN_LR_HAPPY_MS                 3000
#define WIN_SINGLE_THEN_LONG_ANGRY_MS   5000
#define WIN_SINGLE_THEN_DOUBLE_ANGRY_MS 5000
#define WIN_ALT_DANCE_MS                3000
#define WIN_REPEAT3_MS                  3000

/* ================================================================
 *                     手 势 数 据 结 构
 * ================================================================ */
typedef struct {
    touch_btn_key_id_t key_id;
    touch_btn_event_t  event;
    uint32_t           ts;
} gesture_evt_t;

typedef struct {
    gesture_evt_t buf[GESTURE_HISTORY_MAX];
    uint8_t       count;
    uint8_t       focus_type;
    uint32_t      last_ts;
    uint32_t      last_fire_ts;
} gesture_ctx_t;

static gesture_ctx_t g_ctx;

/* ================================================================
 *                     手 势 工 具 函 数
 * ================================================================ */
static inline void safe_play(const char *scene_name)
{
    app_behavior_play_scene(scene_name, NULL, BEHAVIOR_PRIO_DEFAULT);
}

/* 播放指定场景的指定 preset（用于一个场景文件内含多个方向的子动作时） */
static inline void safe_play_preset(const char *scene_name, const char *preset)
{
    app_behavior_play_scene(scene_name, preset, BEHAVIOR_PRIO_DEFAULT);
}

/* 用户主动按键的即时反馈：用交互高优先级，确保能打断正在播放的
 * 普通/情绪场景，避免左右键反馈被排队等待（约 2s 延迟）。 */
static inline void safe_play_focus(const char *scene_name)
{
    app_behavior_play_scene(scene_name, NULL, PRIO_HIGH);
}
static inline uint32_t gesture_now_ms(void)
{
    return get_tick_ms();
}

static void gesture_reset_if_idle(uint32_t ts)
{
    if (g_ctx.count == 0) return;
    if (ts - g_ctx.last_ts > GESTURE_RESET_MS) {
        g_ctx.focus_type = !g_ctx.focus_type;
        g_ctx.count = 0;
    }
}

static void gesture_push(touch_btn_key_id_t key_id, touch_btn_event_t event, uint32_t ts)
{
    gesture_reset_if_idle(ts);
    if (g_ctx.count >= GESTURE_HISTORY_MAX) {
        memmove(&g_ctx.buf[0], &g_ctx.buf[1],
                sizeof(gesture_evt_t) * (GESTURE_HISTORY_MAX - 1));
        g_ctx.count = GESTURE_HISTORY_MAX - 1;
    }
    g_ctx.buf[g_ctx.count++] = (gesture_evt_t){
        .key_id = key_id, .event = event, .ts = ts
    };
    g_ctx.last_ts = ts;
}

static bool within_ms(uint32_t newer, uint32_t older, uint32_t window_ms)
{
    return (newer >= older) && (newer - older <= window_ms);
}

static void play_random_happy(void)
{
    uint32_t r = eyes_rand32() % 3;
    switch (r) {
        case 0: safe_play("happy");     break;
        case 1: safe_play("happy");     break;
        default:safe_play("happy"); break;
    }
}

/* ================================================================
 *                     手 势 规 则 匹 配
 * ================================================================ */
static bool match_single_then_double_angry(uint32_t ts)
{
    if (g_ctx.count < 2) return false;
    gesture_evt_t a = g_ctx.buf[g_ctx.count - 2];
    gesture_evt_t b = g_ctx.buf[g_ctx.count - 1];
    if (a.event == TOUCH_BTN_EVT_CLICK && b.event == TOUCH_BTN_EVT_DOUBLE_CLICK &&
        within_ms(b.ts, a.ts, WIN_SINGLE_THEN_DOUBLE_ANGRY_MS))
    {
        /* 声音由 action_angry_focus_* 场景的 audio 通道统一播放（angry.json），
         * 此处不再直接调用 ApplicationPlaySound，避免与 JSON 重复。 */
        if (b.key_id == TOUCH_BTN_KEY_RIGHT) {
            safe_play("action_angry_focus_left");
        } else {
            safe_play("action_angry_focus_right");
        }
        g_ctx.count = 0;
        g_ctx.last_fire_ts = ts;
        return true;
    }
    return false;
}

static bool match_single_then_long_angry(uint32_t ts)
{
    if (g_ctx.count < 2) return false;
    gesture_evt_t a = g_ctx.buf[g_ctx.count - 2];
    gesture_evt_t b = g_ctx.buf[g_ctx.count - 1];
    if (a.event == TOUCH_BTN_EVT_CLICK && b.event == TOUCH_BTN_EVT_LONG_PRESS &&
        within_ms(b.ts, a.ts, WIN_SINGLE_THEN_LONG_ANGRY_MS))
    {
        // ApplicationSendMessage("你被摸得有点生气了，要表达不耐烦和生气；比如：别摸了别摸了，再摸我就要发火啦！", true);
        int type = eyes_rand32() % 4;

        switch (type) {
        case 0:
            safe_play("angry");
            break;
        case 1:
            safe_play_preset("emotion_angry", "flash_rage");
            break;
        case 2:
            safe_play("action_angry_focus_left");
            break;
        case 3:
            safe_play("action_angry_focus_right");
            break;
        }
        g_ctx.count = 0;
        g_ctx.last_fire_ts = ts;
        return true;
    }
    return false;
}

static bool match_lr_happy(uint32_t ts)
{
    if (g_ctx.count < 2) return false;
    gesture_evt_t a = g_ctx.buf[g_ctx.count - 2];
    gesture_evt_t b = g_ctx.buf[g_ctx.count - 1];
    if (a.event == TOUCH_BTN_EVT_CLICK && b.event == TOUCH_BTN_EVT_CLICK &&
        within_ms(b.ts, a.ts, WIN_LR_HAPPY_MS) &&
        a.key_id != b.key_id)
    {
        play_random_happy();
        g_ctx.count = 0;
        g_ctx.last_fire_ts = ts;
        return true;
    }
    return false;
}

static bool match_long_press_reset(uint32_t ts)
{
    if (g_ctx.count < 1) return false;
    gesture_evt_t b = g_ctx.buf[g_ctx.count - 1];
    if (b.event == TOUCH_BTN_EVT_LONG_PRESS) {
        if(app_ui_logic_get_state() != APP_EVT_STATE_LISTENING && app_ui_logic_get_state() != APP_EVT_STATE_SPEAKING){
            app_behavior_status_set(BEHAVIOR_STATUS_IDLE);
            g_ctx.count = 0;
            g_ctx.last_fire_ts = ts;
        }
        else{
            ApplicationStopChat();
        }

        return true;
    }
    return false;
}

static bool match_double_random_happy(uint32_t ts)
{
    if (g_ctx.count < 1) return false;
    gesture_evt_t b = g_ctx.buf[g_ctx.count - 1];
    if (b.event == TOUCH_BTN_EVT_DOUBLE_CLICK) {
        /* 双击开心：笑声+惊喜音由 game_win 场景统一播放 */
        app_behavior_play_scene("game_win", NULL, BEHAVIOR_PRIO_DEFAULT);
        g_ctx.count = 0;
        g_ctx.last_fire_ts = ts;
        return true;
    }
    return false;
}

static bool match_fast_tap_surprised(uint32_t ts)
{
    if (g_ctx.count < 2) return false;
    gesture_evt_t a = g_ctx.buf[g_ctx.count - 2];
    gesture_evt_t b = g_ctx.buf[g_ctx.count - 1];
    if (a.event == TOUCH_BTN_EVT_CLICK && b.event == TOUCH_BTN_EVT_CLICK &&
        within_ms(b.ts, a.ts, WIN_FAST_TAP_SURPRISED_MS))
    {
        safe_play("surprised");
        g_ctx.count = 0;
        g_ctx.last_fire_ts = ts;
        return true;
    }
    return false;
}

static void game_start(void);
static void game_timeout(void);
static void game_handle_touch(touch_btn_key_id_t key_id, uint32_t ts);

static bool match_alternating_dance(uint32_t ts)
{
    int n = 0;
    uint32_t first_ts = 0, last_ts = 0;
    for (int i = (int)g_ctx.count - 1; i >= 0; --i) {
        if (g_ctx.buf[i].event != TOUCH_BTN_EVT_CLICK) break;
        if (i < (int)g_ctx.count - 1) {
            if (g_ctx.buf[i].key_id == g_ctx.buf[i + 1].key_id) break;
        }
        if (n == 0) first_ts = g_ctx.buf[i].ts;
        last_ts = g_ctx.buf[g_ctx.count - 1].ts;
        n++;
        if (last_ts - first_ts > WIN_ALT_DANCE_MS) break;
    }
    if (n >= 3 && (last_ts - first_ts) <= WIN_ALT_DANCE_MS) {
        game_start();
        g_ctx.last_fire_ts = ts;
        return true;
    }
    return false;
}

static bool match_repeat3_confused_or_thinking(uint32_t ts)
{
    if (g_ctx.count < 3) return false;
    gesture_evt_t b = g_ctx.buf[g_ctx.count - 1];
    gesture_evt_t a = g_ctx.buf[g_ctx.count - 2];
    gesture_evt_t c = g_ctx.buf[g_ctx.count - 3];
    if (b.event == TOUCH_BTN_EVT_CLICK && a.event == TOUCH_BTN_EVT_CLICK && c.event == TOUCH_BTN_EVT_CLICK &&
        b.key_id == a.key_id && a.key_id == c.key_id &&
        within_ms(b.ts, c.ts, WIN_REPEAT3_MS))
    {
        if (b.key_id == TOUCH_BTN_KEY_LEFT) {
            safe_play("curious");
        } else {
            safe_play("thinking");
        }
        g_ctx.count = 0;
        g_ctx.last_fire_ts = ts;
        return true;
    }
    return false;
}

/* ================================================================
 *                     交 替 游 戏 (规则7)
 * ================================================================ */
#define GAME_TIMEOUT_MS     20000
#define GAME_ROUNDS         3

typedef enum {
    GAME_STATE_IDLE,
    GAME_STATE_PLAYING,
} game_state_t;

static game_state_t          s_game_state          = GAME_STATE_IDLE;
static int                   s_game_round          = 0;
static touch_btn_key_id_t    s_game_correct_side   = TOUCH_BTN_KEY_LEFT;
static uint32_t              s_game_round_start_time = 0;

/* 每回合的视觉/舵机/LED 反馈全部由 scenes/sys/game_round_{round}_{side}.json 驱动，
 * 不再直接调用底层硬件接口。 */

static void game_setup_round(int round)
{
    if (round == 0) {
        s_game_correct_side = (eyes_rand32() % 2) ? TOUCH_BTN_KEY_LEFT : TOUCH_BTN_KEY_RIGHT;
    } else {
        s_game_correct_side = (s_game_correct_side == TOUCH_BTN_KEY_LEFT)
                                  ? TOUCH_BTN_KEY_RIGHT
                                  : TOUCH_BTN_KEY_LEFT;
    }

    /* 每回合的虹膜纹理、左右 LED（match/wrong 配色）、眼睛注视方向，
     * 全部由 scenes/sys/game_round_{round}_{side}.json 统一驱动 */
    char scene[32];
    snprintf(scene, sizeof(scene), "game_round_%d_%s",
             round,
             (s_game_correct_side == TOUCH_BTN_KEY_LEFT) ? "left" : "right");
    app_behavior_play_scene(scene, NULL, BEHAVIOR_PRIO_DEFAULT);

    s_game_round_start_time = get_tick_ms();
}

static void game_start(void)
{
    s_game_round = 0;
    g_ctx.count = 0;
    /* 先记录回合起始时间，再翻转状态为 PLAYING：
     * 渲染任务（核1）每 5ms 轮询 timeout 检查，若它先看到 PLAYING 而
     * s_game_round_start_time 仍是旧值（首启为 0），now - 0 > 20s 会立即误判超时。
     * 必须先更新时间戳再放开 PLAYING，消除跨核竞态窗口。 */
    s_game_round_start_time = get_tick_ms();
    s_game_state = GAME_STATE_PLAYING;
    /* 游戏开始：先复位（虹膜/LED/舵机/表情，高优先级立即打断），再进入第 1 回合视觉 */
    app_behavior_play_scene("game_start", NULL, PRIO_HIGH);
    game_setup_round(0);
}

static void game_exit(void)
{
    /* 退出：恢复默认虹膜、关闭 LED、舵机归中、表情复位，全部由 JSON 场景驱动 */
    app_behavior_play_scene("game_exit", NULL, PRIO_HIGH);
    s_game_state = GAME_STATE_IDLE;
    g_ctx.count = 0;
}

static void game_timeout(void)
{
    /* 超时反馈（红 LED/悲伤表情）由 game_timeout 场景统一驱动 */
    app_behavior_play_scene("game_timeout", NULL, PRIO_HIGH);
    game_exit();
}

static void game_handle_touch(touch_btn_key_id_t key_id, uint32_t ts)
{
    (void)ts;

    if (key_id == s_game_correct_side) {
        s_game_round++;
        if (s_game_round >= GAME_ROUNDS) {
            /* 胜利：绿光/庆祝动作/胜利旋律由 game_win 场景统一驱动 */
            app_behavior_play_scene("game_win", NULL, PRIO_HIGH);
            game_exit();
        } else {
            /* 答对一回合：直接进入下一回合视觉，不插入 happy 场景，
             * 避免被下一回合快照立即打断，且避免其无 no_restore 触发
             * 状态恢复把硬件重置为待机，干扰后续回合。 */
            game_setup_round(s_game_round);
        }
    } else {
        /* 答错：红虹膜/红 LED/愤怒表情/握拳由 game_mismatch 场景统一驱动 */
        app_behavior_play_scene("game_mismatch", NULL, PRIO_HIGH);
    }
}

/* ================================================================
 *                     基 础 即 时 反 馈
 * ================================================================ */
static void base_focus_on_single(touch_btn_key_id_t key_id)
{
    if (g_ctx.focus_type) {
        if (key_id == TOUCH_BTN_KEY_LEFT) {
            safe_play_focus("action_focus_left");
        } else {
            safe_play_focus("action_focus_right");
        }
    } else {
        if (key_id == TOUCH_BTN_KEY_LEFT) {
            safe_play_focus("action_focus_top_left");
        } else {
            safe_play_focus("action_focus_top_right");
        }
    }
}

/* ================================================================
 *                     按 键 事 件 主 入 口
 * ================================================================ */
static bool is_alt_pattern_accumulating(void)
{
    if (g_ctx.count < 3) return false;
    gesture_evt_t a = g_ctx.buf[g_ctx.count - 2];
    gesture_evt_t b = g_ctx.buf[g_ctx.count - 1];
    return (a.event == TOUCH_BTN_EVT_CLICK &&
            b.event == TOUCH_BTN_EVT_CLICK &&
            a.key_id != b.key_id &&
            within_ms(b.ts, a.ts, WIN_ALT_DANCE_MS));
}

void logic_page_eyes_reset_state(void)
{
    memset(&g_ctx, 0, sizeof(gesture_ctx_t));
    s_game_state = GAME_STATE_IDLE;
}

bool logic_page_eyes_is_game_playing(void)
{
    return s_game_state == GAME_STATE_PLAYING;
}

uint32_t logic_page_eyes_game_round_start_time(void)
{
    return s_game_round_start_time;
}

void logic_page_eyes_game_timeout_check(void)
{
    if (s_game_state == GAME_STATE_PLAYING) {
        uint32_t now = get_tick_ms();
        if (now - s_game_round_start_time > GAME_TIMEOUT_MS) {
            game_timeout();
        }
    }
}

void logic_page_eyes_on_key(int key_id, touch_btn_event_t event)
{
    uint32_t t = gesture_now_ms();

    if (key_id != TOUCH_BTN_KEY_LEFT && key_id != TOUCH_BTN_KEY_RIGHT) return;
    if (event >= TOUCH_BTN_EVT_MAX) return;

    if (s_game_state == GAME_STATE_PLAYING) {
        if (event == TOUCH_BTN_EVT_CLICK) {
            game_handle_touch((touch_btn_key_id_t)key_id, t);
        }
        return;
    }

    if (t - g_ctx.last_fire_ts < GESTURE_COOLDOWN_MS) {
        gesture_push(key_id, event, t);
        if (event == TOUCH_BTN_EVT_CLICK && !is_alt_pattern_accumulating()) {
            base_focus_on_single(key_id);
        }return;
    }

    gesture_push(key_id, event, t);

    if (event == TOUCH_BTN_EVT_CLICK && !is_alt_pattern_accumulating()) {
        base_focus_on_single(key_id);
    }

    if (match_single_then_double_angry(t)) return;
    if (match_single_then_long_angry(t)) return;
    if (match_long_press_reset(t)) return;
    if (match_double_random_happy(t)) return;
    if (match_alternating_dance(t)) return;
    if (match_repeat3_confused_or_thinking(t)) return;
}
