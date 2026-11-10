#include "behavior_xx_parse.h"
#include "eyes_control.h"
#include "eyes_core.h"

#include <stdlib.h>
#include <string.h>

/* ---------- 名称 → EYES_EMOTION ---------- */
static int eyes_emotion_from_name(const char *name)
{
    if (!name) return -1;
    static const struct { const char *n; EYES_EMOTION e; } map[] = {
        {"default", EYES_EMOTION_DEFAULT},
        {"focus_below_center", EYES_EMOTION_FOCUS_BELOW_CENTER},
        {"focus_top_center", EYES_EMOTION_FOCUS_TOP_CENTER},
        {"focus_top_left", EYES_EMOTION_FOCUS_TOP_LEFT},
        {"focus_top_right", EYES_EMOTION_FOCUS_TOP_RIGHT},
        {"focus_left", EYES_EMOTION_FOCUS_LEFT},
        {"focus_right", EYES_EMOTION_FOCUS_RIGHT},
        {"happy", EYES_EMOTION_HAPPY},
        {"happy1", EYES_EMOTION_HAPPY1},
        {"happy2", EYES_EMOTION_HAPPY2},
        {"angry", EYES_EMOTION_ANGRY},
        {"angry1", EYES_EMOTION_ANGRY1},
        {"angry_right", EYES_EMOTION_ANGRY_RIGHT},
        {"angry_left", EYES_EMOTION_ANGRY_LEFT},
        {"sad", EYES_EMOTION_SAD},
        {"suspicious", EYES_EMOTION_SUSPICIOUS},
        {"amazed", EYES_EMOTION_AMAZED},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(name, map[i].n) == 0) return (int)map[i].e;
    }
    return -1;
}

/* ---------- 播放数据 ---------- */
typedef enum {
    EYE_OP_EMOTION = 0,
    EYE_OP_POS,
} eye_op_t;

typedef struct {
    eye_op_t op;
    EYES_EMOTION emotion;   /* EYE_OP_EMOTION */
    float pos_x;            /* EYE_OP_POS */
    float pos_y;
    int32_t pos_dur_ms;
    uint32_t time_ms;       /* 本关键帧在单遍内的起始时间戳(ms) */
    uint32_t hold_ms;       /* 本关键帧持续时长(ms)，用于推算单遍总时长 */
} eye_kf_t;

typedef struct {
    eye_kf_t *kfs;
    int       count;
    uint32_t  span_ms;      /* 单遍总时长(ms)，>=1 */
} eye_play_t;

/* 表情型关键帧默认保持时长：避免被管理器判定为“瞬时完成”而立即被状态基线覆盖 */
#define EYE_EMOTION_HOLD_MS  1000

static eye_kf_t *eye_kf_parse(const cJSON *kf, uint32_t time_ms)
{
    eye_kf_t *e = (eye_kf_t *)calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->op = EYE_OP_EMOTION;
    e->emotion = EYES_EMOTION_DEFAULT;
    e->pos_x = 0.0f;
    e->pos_y = 0.0f;
    e->pos_dur_ms = 300;
    e->time_ms = time_ms;
    e->hold_ms = EYE_EMOTION_HOLD_MS;

    /* 注视位置：两种写法都支持
     *   A) 嵌套对象：{ "pos": { "x": .., "y": .. }, "duration_ms": .. }
     *   B) 扁平字段：{ "move_x": .., "move_y": .., "duration_ms": .. }   (eye_move.json)
     * 对应 C 动作库：eye_controller_set_target_position(x, y, dur) */
    float mx = 0.0f, my = 0.0f;
    bool has_pos = false;

    const cJSON *pos = cJSON_GetObjectItemCaseSensitive(kf, "pos");
    if (cJSON_IsObject(pos)) {
        double x = 0.0, y = 0.0;
        behavior_kf_get_f64(pos, "x", &x);
        behavior_kf_get_f64(pos, "y", &y);
        mx = (float)x;
        my = (float)y;
        has_pos = true;
    } else {
        double x = 0.0, y = 0.0;
        if (behavior_kf_get_f64(kf, "move_x", &x) || behavior_kf_get_f64(kf, "move_y", &y)) {
            mx = (float)x;
            my = (float)y;
            has_pos = true;
        }
    }

    if (has_pos) {
        e->op = EYE_OP_POS;
        e->pos_x = mx;
        e->pos_y = my;
        int64_t dur = 300;
        behavior_kf_get_i64(kf, "duration_ms", &dur);
        e->pos_dur_ms = (int32_t)(dur < 0 ? 0 : (dur > 0x7FFFFFFF ? 0x7FFFFFFF : dur));
        e->hold_ms = (uint32_t)(e->pos_dur_ms > 0 ? e->pos_dur_ms : 300);
        /* 位置关键帧也要读取自身的 time_ms，否则会被调度器当作 time_ms=0
         * 而和同 asset 内其它关键帧在同一 tick 全部触发，导致错峰移动失效 */
        uint32_t kf_time = time_ms;
        behavior_kf_get_u32(kf, "time_ms", &kf_time);
        e->time_ms = kf_time;
        return e;
    }

    /* 表情：{ "expr": "happy" }；time_ms 优先取 JSON 内字段 */
    uint32_t kf_time = time_ms;
    behavior_kf_get_u32(kf, "time_ms", &kf_time);
    e->time_ms = kf_time;

    const cJSON *expr = cJSON_GetObjectItemCaseSensitive(kf, "expr");
    if (cJSON_IsString(expr) && expr->valuestring) {
        int emo = eyes_emotion_from_name(expr->valuestring);
        if (emo < 0) { free(e); return NULL; }
        e->emotion = (EYES_EMOTION)emo;
        /* 允许 JSON 用 duration_ms 指定表情保持时长 */
        uint32_t hold = 0;
        if (behavior_kf_get_u32(kf, "duration_ms", &hold) && hold > 0)
            e->hold_ms = hold;
        return e;
    }

    free(e);
    return NULL;
}

static void *eye_parse(const cJSON *asset)
{
    const cJSON *kfs = cJSON_GetObjectItemCaseSensitive(asset, "keyframes");
    if (!cJSON_IsArray(kfs) || cJSON_GetArraySize(kfs) < 1) return NULL;

    int n = cJSON_GetArraySize(kfs);
    eye_play_t *p = (eye_play_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->kfs = (eye_kf_t *)calloc((size_t)n, sizeof(eye_kf_t));
    if (!p->kfs) { free(p); return NULL; }

    uint32_t span = 0;
    for (int i = 0; i < n; i++) {
        const cJSON *kf = cJSON_GetArrayItem(kfs, i);
        eye_kf_t *e = eye_kf_parse(kf, 0);
        if (!e) continue;   /* 跳过不可解析的关键帧 */
        p->kfs[p->count] = *e;
        free(e);
        p->count++;
        uint32_t end = p->kfs[p->count - 1].time_ms + p->kfs[p->count - 1].hold_ms;
        if (end > span) span = end;
    }
    p->span_ms = span < 1 ? 1 : span;

    if (p->count < 1) {
        free(p->kfs);
        free(p);
        return NULL;
    }
    return p;
}

static int eye_keyframes(const void *play)
{
    const eye_play_t *p = (const eye_play_t *)play;
    return p ? p->count : 0;
}

static uint32_t eye_kf_time(const void *play, int idx)
{
    const eye_play_t *p = (const eye_play_t *)play;
    if (!p || idx < 0 || idx >= p->count) return 0;
    return p->kfs[idx].time_ms;
}

static void eye_apply(void *play, int idx)
{
    const eye_play_t *p = (const eye_play_t *)play;
    if (!p || idx < 0 || idx >= p->count) return;
    const eye_kf_t *e = &p->kfs[idx];
    if (e->op == EYE_OP_POS) {
        eye_controller_set_target_position(e->pos_x, e->pos_y, e->pos_dur_ms);
    } else {
        eye_controller_apply_emotion(e->emotion);
    }
}

static uint32_t eye_duration(const void *play)
{
    const eye_play_t *p = (const eye_play_t *)play;
    return p ? p->span_ms : 1;   /* 单遍总时长，含表情保持时间，避免被瞬时判定完成 */
}

static void eye_destroy(void *play, bool interrupted)
{
    (void)interrupted;   /* 表情复位交给 status 模块 */
    eye_play_t *p = (eye_play_t *)play;
    if (p) free(p->kfs);
    free(play);
}

/* 无硬件需复位（表情由 status 模块控制） */
static void eye_stop(void *play, bool interrupted)
{
    (void)play;
    (void)interrupted;
}

const behavior_channel_t behavior_channel_eye = {
    .name = "eye",
    .parse = eye_parse,
    .keyframe_count = eye_keyframes,
    .kf_time_ms = eye_kf_time,
    .apply = eye_apply,
    .duration_ms = eye_duration,
    .destroy = eye_destroy,
    .stop = eye_stop,
};