#include "behavior_xx_parse.h"
#include "app_servo.h"

#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"

/* ---------- 播放数据 ---------- */
typedef struct {
    uint16_t n;
    struct {
        uint32_t t;        /* 本地时间戳(ms) */
        uint8_t  left;     /* 左臂角度 */
        uint8_t  right;    /* 右臂角度 */
        uint16_t dur;      /* 运动耗时(ms) */
    } *kf;
} servo_play_t;

static void *servo_parse(const cJSON *asset)
{
    const cJSON *kfs = cJSON_GetObjectItemCaseSensitive(asset, "keyframes");
    if (!cJSON_IsArray(kfs)) return NULL;
    int n = cJSON_GetArraySize(kfs);
    if (n <= 0) return NULL;

    servo_play_t *p = (servo_play_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    /* 关键帧数组可能较大，放 PSRAM */
    p->kf = (typeof(p->kf))heap_caps_calloc((size_t)n, sizeof(*p->kf),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p->kf) { free(p); return NULL; }
    p->n = (uint16_t)n;

    for (int i = 0; i < n; i++) {
        const cJSON *kf = cJSON_GetArrayItem(kfs, i);
        behavior_kf_get_u32(kf, "time_ms", &p->kf[i].t);

        int16_t l = 90, r = 90;
        behavior_kf_get_i16(kf, "left_arm", &l);
        behavior_kf_get_i16(kf, "right_arm", &r);
        p->kf[i].left  = (uint8_t)(l < 0 ? 0 : (l > 180 ? 180 : l));
        p->kf[i].right = (uint8_t)(r < 0 ? 0 : (r > 180 ? 180 : r));

        uint32_t dur = 500;
        behavior_kf_get_duration_ms(kf, &dur);
        p->kf[i].dur = (uint16_t)(dur > 0xFFFF ? 0xFFFF : dur);
    }
    return p;
}

static int servo_keyframes(const void *play)
{
    return (int)((const servo_play_t *)play)->n;
}

static uint32_t servo_kf_time(const void *play, int idx)
{
    return ((const servo_play_t *)play)->kf[idx].t;
}

static void servo_apply(void *play, int idx)
{
    const servo_play_t *p = (const servo_play_t *)play;
    app_servo_move_sync((float)p->kf[idx].left, (float)p->kf[idx].right, p->kf[idx].dur);
}

static uint32_t servo_duration(const void *play)
{
    const servo_play_t *p = (const servo_play_t *)play;
    if (p->n == 0) return 1;
    return p->kf[p->n - 1].t + p->kf[p->n - 1].dur;
}

static void servo_destroy(void *play, bool interrupted)
{
    (void)interrupted;   /* 双臂归位由 status 模块显式控制，这里只释放 */
    servo_play_t *p = (servo_play_t *)play;
    free(p->kf);
    free(p);
}

/* 双臂归位由 status 模块显式控制，无额外硬件复位 */
static void servo_stop(void *play, bool interrupted)
{
    (void)play;
    (void)interrupted;
}

const behavior_channel_t behavior_channel_servo = {
    .name = "servo",
    .parse = servo_parse,
    .keyframe_count = servo_keyframes,
    .kf_time_ms = servo_kf_time,
    .apply = servo_apply,
    .duration_ms = servo_duration,
    .destroy = servo_destroy,
    .stop = servo_stop,
};