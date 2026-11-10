#include "behavior_xx_parse.h"
#include "app_ui.h"       /* ApplicationPlaySound / SoundEffect_t */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---------- clip 名 → SoundEffect_t 映射（可按实际音效表扩充） ---------- */
typedef struct { const char *name; int sound; uint16_t dur_ms; } audio_map_t;

static const audio_map_t s_audio_map[] = {
    { "sad",     SOUND_6812623,     900 },
    { "angry",   SOUND_1067936557,  900 },
    { "surprised", SOUND_1068702996, 700 },
    { "happy",   SOUND_1724636,     700 },
    { "laugh",   SOUND_1067002988,  700 },
    { "wow",     SOUND_1068702996,  700 },
    { "tick",    SOUND_TICKING_CLOCK, 500 },
    { "ticking", SOUND_TICKING_CLOCK, 500 },
};

static int sound_from_clip(const char *clip, uint16_t *dur_out)
{
    *dur_out = 1000;   /* 默认时长 */
    if (!clip) return SOUND_STOP;

    for (size_t i = 0; i < sizeof(s_audio_map) / sizeof(s_audio_map[0]); i++) {
        if (strcmp(clip, s_audio_map[i].name) == 0) {
            *dur_out = s_audio_map[i].dur_ms;
            return s_audio_map[i].sound;
        }
    }

    /* 兼容纯数字文件名，如 clip="6812623" */
    if (isdigit((unsigned char)clip[0])) {
        unsigned long v = strtoul(clip, NULL, 10);
        switch (v) {
        case 1724636:    *dur_out = 700;  return SOUND_1724636;
        case 6812623:    *dur_out = 900;  return SOUND_6812623;
        case 1067002988: *dur_out = 700;  return SOUND_1067002988;
        case 1067936557: *dur_out = 700;  return SOUND_1067936557;
        case 1068702996: *dur_out = 700;  return SOUND_1068702996;
        default: break;
        }
    }
    return SOUND_STOP;
}

/* ---------- 播放数据 ---------- */
typedef struct {
    int      sound;
    uint16_t dur_ms;
} audio_play_t;

static void *audio_parse(const cJSON *asset)
{
    const cJSON *kfs = cJSON_GetObjectItemCaseSensitive(asset, "keyframes");
    if (!cJSON_IsArray(kfs) || cJSON_GetArraySize(kfs) < 1) return NULL;
    const cJSON *kf0 = cJSON_GetArrayItem(kfs, 0);
    const cJSON *clip = cJSON_GetObjectItemCaseSensitive(kf0, "clip");
    if (!cJSON_IsString(clip) || !clip->valuestring) return NULL;

    uint16_t dur = 1000;
    int sound = sound_from_clip(clip->valuestring, &dur);
    if (sound == SOUND_STOP) return NULL;   /* 未知音效则视为无效资产 */

    audio_play_t *p = (audio_play_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->sound = sound;
    p->dur_ms = dur;
    return p;
}

static int audio_keyframes(const void *play)
{
    (void)play;
    return 1;
}

static uint32_t audio_kf_time(const void *play, int idx)
{
    (void)play;
    (void)idx;
    return 0;
}

static void audio_apply(void *play, int idx)
{
    (void)idx;
    audio_play_t *p = (audio_play_t *)play;
    ApplicationPlaySound((SoundEffect_t)p->sound);
}

static uint32_t audio_duration(const void *play)
{
    return ((const audio_play_t *)play)->dur_ms;
}

static void audio_destroy(void *play, bool interrupted)
{
    (void)interrupted;
    free(play);
}

/* 仅停止硬件（不释放），play 可能来自 PSRAM 驻留缓存 */
static void audio_stop(void *play, bool interrupted)
{
    (void)play;
    if (interrupted) ApplicationPlaySound(SOUND_STOP);   /* 打断则停掉当前音效 */
}

const behavior_channel_t behavior_channel_audio = {
    .name = "audio",
    .parse = audio_parse,
    .keyframe_count = audio_keyframes,
    .kf_time_ms = audio_kf_time,
    .apply = audio_apply,
    .duration_ms = audio_duration,
    .destroy = audio_destroy,
    .stop = audio_stop,
};