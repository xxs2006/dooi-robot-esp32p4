#include "behavior_xx_parse.h"
#include "app_motor.h"

#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"

/* ---------- 播放数据 ---------- */
typedef struct {
    uint16_t n;
    struct {
        uint32_t t;
        int16_t  speed_l;
        int16_t  speed_r;
        uint16_t dur;
    } *kf;
} motor_play_t;

static void *motor_parse(const cJSON *asset)
{
    const cJSON *kfs = cJSON_GetObjectItemCaseSensitive(asset, "keyframes");
    if (!cJSON_IsArray(kfs)) return NULL;
    int n = cJSON_GetArraySize(kfs);
    if (n <= 0) return NULL;

    motor_play_t *p = (motor_play_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    /* 关键帧数组可能较大，放 PSRAM */
    p->kf = (typeof(p->kf))heap_caps_calloc((size_t)n, sizeof(*p->kf),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p->kf) { free(p); return NULL; }
    p->n = (uint16_t)n;

    for (int i = 0; i < n; i++) {
        const cJSON *kf = cJSON_GetArrayItem(kfs, i);
        behavior_kf_get_u32(kf, "time_ms", &p->kf[i].t);
        behavior_kf_get_i16(kf, "speed_l", &p->kf[i].speed_l);
        behavior_kf_get_i16(kf, "speed_r", &p->kf[i].speed_r);
        uint32_t dur = 100;
        behavior_kf_get_duration_ms(kf, &dur);
        p->kf[i].dur = (uint16_t)(dur > 0xFFFF ? 0xFFFF : dur);
    }
    return p;
}

static int motor_keyframes(const void *play)
{
    return (int)((const motor_play_t *)play)->n;
}

static uint32_t motor_kf_time(const void *play, int idx)
{
    return ((const motor_play_t *)play)->kf[idx].t;
}

static void motor_apply(void *play, int idx)
{
    const motor_play_t *p = (const motor_play_t *)play;
    app_motor_move_auto(p->kf[idx].speed_l, p->kf[idx].speed_r, p->kf[idx].dur);
}

static uint32_t motor_duration(const void *play)
{
    const motor_play_t *p = (const motor_play_t *)play;
    if (p->n == 0) return 1;
    return p->kf[p->n - 1].t + p->kf[p->n - 1].dur;
}

static void motor_destroy(void *play, bool interrupted)
{
    (void)interrupted;
    motor_play_t *p = (motor_play_t *)play;
    free(p->kf);
    free(p);
}

/* 仅复位硬件（不释放）：断电安全，结束/打断都停电机 */
static void motor_stop(void *play, bool interrupted)
{
    (void)play;
    (void)interrupted;
    app_motor_stop();
}

const behavior_channel_t behavior_channel_motor = {
    .name = "motor",
    .parse = motor_parse,
    .keyframe_count = motor_keyframes,
    .kf_time_ms = motor_kf_time,
    .apply = motor_apply,
    .duration_ms = motor_duration,
    .destroy = motor_destroy,
    .stop = motor_stop,
};