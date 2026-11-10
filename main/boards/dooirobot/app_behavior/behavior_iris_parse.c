#include "behavior_xx_parse.h"
#include "eyes_core.h"      // eye_controller_apply_iris_texture / EYES_IRIS_DEFINE
#include "app_behavior_fs.h"

#include <stdlib.h>
#include <string.h>

/*
 * 虹膜纹理通道：把游戏/表情需要的虹膜纹理切换到指定类型。
 * 资产字段：
 *   iris_type : int  -> EYES_IRIS_DEFINE 枚举值
 */

typedef struct {
    int iris_type;
} behavior_iris_play_t;

static void *iris_parse(const cJSON *asset)
{
    if (!asset) return NULL;
    behavior_iris_play_t *p = (behavior_iris_play_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    int32_t t = 0;
    if (behavior_kf_get_i32(asset, "iris_type", &t)) {
        p->iris_type = t;
    } else {
        /* 兼容字符串形式：default/emerald/violet/blue/amber/gray/ruby/winered */
        const cJSON *it = cJSON_GetObjectItemCaseSensitive(asset, "iris");
        if (it && cJSON_IsString(it) && it->valuestring) {
            const char *s = it->valuestring;
            if      (!strcmp(s, "default")) p->iris_type = EYES_IRIS_WINERED;
            else if (!strcmp(s, "emerald")) p->iris_type = EYES_IRIS_EMERALD;
            else if (!strcmp(s, "violet"))  p->iris_type = EYES_IRIS_VIOLET;
            else if (!strcmp(s, "blue"))    p->iris_type = EYES_IRIS_BLUE;
            else if (!strcmp(s, "amber"))   p->iris_type = EYES_IRIS_AMBER;
            else if (!strcmp(s, "gray"))    p->iris_type = EYES_IRIS_GRAY;
            else if (!strcmp(s, "ruby"))    p->iris_type = EYES_IRIS_RUBY;
            else if (!strcmp(s, "winered")) p->iris_type = EYES_IRIS_WINERED;
            else p->iris_type = EYES_IRIS_DEFAULT;
        } else {
            p->iris_type = EYES_IRIS_DEFAULT;
        }
    }
    return p;
}

static int iris_keyframe_count(const void *play)
{
    (void)play;
    return 1;
}

static uint32_t iris_kf_time_ms(const void *play, int idx)
{
    (void)play; (void)idx;
    return 0;
}

static void iris_apply(void *play, int idx)
{
    (void)idx;
    behavior_iris_play_t *p = (behavior_iris_play_t *)play;
    if (!p) return;
    eye_controller_apply_iris_texture((EYES_IRIS_DEFINE)p->iris_type);
}

static uint32_t iris_duration_ms(const void *play)
{
    (void)play;
    return 1; /* 瞬时动作，单帧 */
}

static void iris_destroy(void *play, bool interrupted)
{
    (void)interrupted;
    free(play);
}

/* 瞬时纹理切换，无硬件复位 */
static void iris_stop(void *play, bool interrupted)
{
    (void)play;
    (void)interrupted;
}

const behavior_channel_t behavior_channel_iris = {
    .name          = "iris",
    .parse         = iris_parse,
    .keyframe_count= iris_keyframe_count,
    .kf_time_ms    = iris_kf_time_ms,
    .apply         = iris_apply,
    .duration_ms   = iris_duration_ms,
    .destroy       = iris_destroy,
    .stop          = iris_stop,
};
