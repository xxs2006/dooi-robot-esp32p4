#include "behavior_xx_parse.h"
#include "app_behavior_fs.h"

#include <stdio.h>
#include <string.h>

static const behavior_channel_t *const s_channels[] = {
    &behavior_channel_eye,
    &behavior_channel_led,
    &behavior_channel_servo,
    &behavior_channel_motor,
    &behavior_channel_audio,
    &behavior_channel_iris,
};

const behavior_channel_t *behavior_channel_find(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(s_channels) / sizeof(s_channels[0]); i++) {
        if (strcmp(s_channels[i]->name, name) == 0) return s_channels[i];
    }
    return NULL;
}

bool behavior_kf_get_u32(const cJSON *kf, const char *key, uint32_t *out)
{
    if (!out) return false;
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(kf, key);
    if (!it || !cJSON_IsNumber(it)) return false;
    *out = (uint32_t)it->valueint;
    return true;
}

bool behavior_kf_get_i16(const cJSON *kf, const char *key, int16_t *out)
{
    if (!out) return false;
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(kf, key);
    if (!it || !cJSON_IsNumber(it)) return false;
    *out = (int16_t)it->valueint;
    return true;
}

bool behavior_kf_get_i32(const cJSON *kf, const char *key, int32_t *out)
{
    if (!out) return false;
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(kf, key);
    if (!it || !cJSON_IsNumber(it)) return false;
    *out = (int32_t)it->valueint;
    return true;
}

bool behavior_kf_get_i64(const cJSON *kf, const char *key, int64_t *out)
{
    if (!out) return false;
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(kf, key);
    if (!it || !cJSON_IsNumber(it)) return false;
    *out = (int64_t)it->valuedouble;
    return true;
}

bool behavior_kf_get_f64(const cJSON *kf, const char *key, double *out)
{
    if (!out) return false;
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(kf, key);
    if (!it || !cJSON_IsNumber(it)) return false;
    *out = (double)it->valuedouble;
    return true;
}

bool behavior_kf_get_duration_ms(const cJSON *kf, uint32_t *out)
{
    if (behavior_kf_get_u32(kf, "duration_ms", out)) return true;
    return behavior_kf_get_u32(kf, "duration", out);
}

bool behavior_parse_color(const cJSON *kf, const char *key, uint32_t *rgb_out)
{
    if (!rgb_out) return false;
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(kf, key);
    if (!c) return false;
    if (cJSON_IsNumber(c)) { *rgb_out = (uint32_t)c->valueint; return true; }
    if (!cJSON_IsString(c) || !c->valuestring) return false;

    const char *s = c->valuestring;
    if (!s || s[0] == '\0') return false;
    if (s[0] == '#') {
        unsigned int v = 0;
        if (sscanf(s + 1, "%x", &v) != 1) return false;
        *rgb_out = (uint32_t)v;
        return true;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        unsigned int v = 0;
        if (sscanf(s + 2, "%x", &v) != 1) return false;
        *rgb_out = (uint32_t)v;
        return true;
    }

    static const struct { const char *name; uint32_t rgb; } t[] = {
        {"red", 0xFF0000}, {"green", 0x00FF00}, {"blue", 0x0000FF},
        {"yellow", 0xFFFF00}, {"cyan", 0x00FFFF}, {"magenta", 0xFF00FF},
        {"white", 0xFFFFFF}, {"black", 0x000000},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (strcmp(s, t[i].name) == 0) { *rgb_out = t[i].rgb; return true; }
    }
    return false;
}

bool behavior_parse_channel_name(const cJSON *timeline_item, const char **out)
{
    if (!out) return false;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(timeline_item, "type");
    if (!t) t = cJSON_GetObjectItemCaseSensitive(timeline_item, "channel");
    if (!t || !cJSON_IsString(t) || !t->valuestring) return false;
    *out = t->valuestring;
    return true;
}