#include "behavior_xx_parse.h"
#include "app_led.h"

#include <stdlib.h>
#include <string.h>

/* ---------- LED 模式 ---------- */
typedef enum {
    LED_MODE_OFF = 0,
    LED_MODE_SOLID,
    LED_MODE_BREATH,
    LED_MODE_BLINK,
    LED_MODE_RAINBOW,
} led_mode_t;

static led_mode_t led_mode_from_str(const char *s)
{
    if (!s) return LED_MODE_OFF;
    if (strcmp(s, "off") == 0)                      return LED_MODE_OFF;
    if (strcmp(s, "solid") == 0 || strcmp(s, "rgb") == 0 || strcmp(s, "set") == 0)
        return LED_MODE_SOLID;
    if (strcmp(s, "breath") == 0 || strcmp(s, "breathing") == 0) return LED_MODE_BREATH;
    if (strcmp(s, "blink") == 0)                    return LED_MODE_BLINK;
    if (strcmp(s, "rainbow") == 0)                  return LED_MODE_RAINBOW;
    return LED_MODE_OFF;
}

/* ---------- 播放数据 ---------- */
typedef struct {
    led_mode_t mode;
    uint32_t   left_color;
    uint32_t   right_color;
    uint16_t   period_ms;
    bool       symmetric;
} led_play_t;

/* 解析左右颜色：优先 left_color/right_color，否则回退到单个 color（两侧同色） */
static void led_parse_colors(const cJSON *kf, led_play_t *p)
{
    uint32_t left = 0, right = 0;
    bool has_left  = behavior_parse_color(kf, "left_color",  &left);
    bool has_right = behavior_parse_color(kf, "right_color", &right);

    if (has_left || has_right) {
        p->left_color  = has_left  ? left  : 0;
        p->right_color = has_right ? right : 0;
        return;
    }

    /* 回退：单个 color 字段，左右同色 */
    uint32_t color = 0;
    if (behavior_parse_color(kf, "color", &color)) {
        p->left_color  = color;
        p->right_color = color;
    }
}

static void *led_parse(const cJSON *asset)
{
    const cJSON *kfs = cJSON_GetObjectItemCaseSensitive(asset, "keyframes");
    if (!cJSON_IsArray(kfs) || cJSON_GetArraySize(kfs) < 1) return NULL;
    const cJSON *kf0 = cJSON_GetArrayItem(kfs, 0);

    const cJSON *m = cJSON_GetObjectItemCaseSensitive(kf0, "mode");
    const char *mode_str = (m && cJSON_IsString(m)) ? m->valuestring : "off";

    led_play_t *p = (led_play_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->mode = led_mode_from_str(mode_str);
    p->symmetric = true;

    led_parse_colors(kf0, p);
    uint32_t period = 1000;
    if (behavior_kf_get_u32(kf0, "period_ms", &period)) p->period_ms = (uint16_t)period;
    const cJSON *sym = cJSON_GetObjectItemCaseSensitive(kf0, "symmetric");
    if (sym && cJSON_IsBool(sym)) p->symmetric = cJSON_IsTrue(sym);
    return p;
}

static int led_keyframes(const void *play)
{
    (void)play;
    return 1;
}

static uint32_t led_kf_time(const void *play, int idx)
{
    (void)play;
    (void)idx;
    return 0;
}

static void led_apply(void *play, int idx)
{
    (void)idx;
    led_play_t *p = (led_play_t *)play;
    switch (p->mode) {
    case LED_MODE_OFF:     app_led_off(); break;
    case LED_MODE_SOLID:   app_led_set_rgb(p->left_color, p->right_color); break;
    case LED_MODE_BREATH:  app_led_set_breathing(p->left_color, p->period_ms, p->symmetric); break;
    case LED_MODE_BLINK:   app_led_set_blink(p->left_color, p->period_ms, p->symmetric); break;
    case LED_MODE_RAINBOW: app_led_set_rainbow(p->period_ms, p->symmetric); break;
    }
}

static uint32_t led_duration(const void *play)
{
    const led_play_t *p = (const led_play_t *)play;
    if (p->mode == LED_MODE_BREATH || p->mode == LED_MODE_BLINK || p->mode == LED_MODE_RAINBOW) {
        return p->period_ms > 0 ? p->period_ms : 1;
    }
    return 1;   /* 瞬时状态型 */
}

static void led_destroy(void *play, bool interrupted)
{
    (void)interrupted;
    free(play);
}

/* 仅复位硬件（不释放）：结束/打断关闭 LED。实心(solid)作为持久状态保留。 */
static void led_stop(void *play, bool interrupted)
{
    (void)interrupted;
    const led_play_t *p = (const led_play_t *)play;
    if (!p || p->mode != LED_MODE_SOLID) app_led_off();
}

const behavior_channel_t behavior_channel_led = {
    .name = "led",
    .parse = led_parse,
    .keyframe_count = led_keyframes,
    .kf_time_ms = led_kf_time,
    .apply = led_apply,
    .duration_ms = led_duration,
    .destroy = led_destroy,
    .stop = led_stop,
};