#include "app_behavior_status.h"
#include "app_behavior.h"

#include "esp_log.h"

#include <string.h>

static const char *TAG = "beh_status";

/* 枚举值与资源文件名的映射表（顺序须与 behavior_status_id_t 一致）。
 * 新增状态时：在枚举中追加并在此处同步追加一行即可，编译期即可保证不漏。 */
static const char *const s_status_names[BEHAVIOR_STATUS_COUNT] = {
    [BEHAVIOR_STATUS_IDLE]            = "status_idle",
    [BEHAVIOR_STATUS_STANDBY]         = "status_standby",
    [BEHAVIOR_STATUS_PAGE_MUSIC]      = "status_on_music",
};

const char *app_behavior_status_name(behavior_status_id_t id)
{
    if ((int)id < 0 || id >= BEHAVIOR_STATUS_COUNT) {
        return s_status_names[BEHAVIOR_STATUS_IDLE];
    }
    return s_status_names[id];
}

bool app_behavior_status_from_name(const char *name, behavior_status_id_t *out_id)
{
    if (!name || !name[0] || !out_id) return false;
    for (int i = 0; i < BEHAVIOR_STATUS_COUNT; i++) {
        if (strcmp(s_status_names[i], name) == 0) {
            *out_id = (behavior_status_id_t)i;
            return true;
        }
    }
    return false;
}

/* 内部状态机：全部用枚举表达，仅在真正播放时才经映射表转成资源文件名，
 * 彻底消除原来 char[] 栈的零散拷贝与字符串字面量硬编码。 */
typedef struct {
    behavior_status_id_t current;                       /* 长期恢复目标状态 */
    behavior_status_id_t before_scene;                  /* 进入 scene 前保存的状态 */
    bool                 has_before_scene;              /* before_scene 是否有效 */
    behavior_status_id_t stack[BEHAVIOR_STATUS_STACK_DEPTH];
    uint8_t              depth;
} status_fsm_t;

static status_fsm_t s_fsm;
static bool s_inited = false;

static void play_status(behavior_status_id_t id)
{
    const char *name = app_behavior_status_name(id);
    if (!app_behavior_play_status(name)) {
        ESP_LOGW(TAG, "play status '%s' failed", name);
    }
}

static void on_scene_started(const char *scene_name, const char *preset)
{
    /* 记录进入 scene 前的当前状态，scene 结束后恢复回它 */
    s_fsm.before_scene = s_fsm.current;
    s_fsm.has_before_scene = true;
    // ESP_LOGI(TAG, "scene '%s' started, save before_scene='%s'",
    //          scene_name, app_behavior_status_name(s_fsm.before_scene));
}

static void on_scene_ended(const behavior_result_t *result, const char *restore_status)
{
    behavior_status_id_t restore = BEHAVIOR_STATUS_IDLE;

    /* 优先级：场景显式指定 restore_status(字符串) > 进入 scene 前的状态 > idle */
    if (restore_status && restore_status[0]) {
        behavior_status_id_t parsed;
        if (app_behavior_status_from_name(restore_status, &parsed)) {
            restore = parsed;
            // ESP_LOGI(TAG, "scene '%s' ended, restore '%s' (explicit)",
            //          result->name, app_behavior_status_name(restore));
        } else {
            ESP_LOGW(TAG, "scene '%s' ended, unknown restore '%s', fallback idle",
                     result->name, restore_status);
        }
    } else if (s_fsm.has_before_scene) {
        restore = s_fsm.before_scene;
        // ESP_LOGI(TAG, "scene '%s' ended, restore '%s' (before_scene)",
        //          result->name, app_behavior_status_name(restore));
    } else {
        // ESP_LOGI(TAG, "scene '%s' ended, restore 'status_idle' (default)", result->name);
    }

    s_fsm.current = restore;
    s_fsm.has_before_scene = false;

    play_status(restore);
}

void app_behavior_status_init(void)
{
    memset(&s_fsm, 0, sizeof(s_fsm));   /* 枚举 0 == BEHAVIOR_STATUS_IDLE */
    app_behavior_set_scene_ended_hook(on_scene_ended);
    app_behavior_set_scene_started_hook(on_scene_started);

    s_inited = true;

    app_behavior_status_push(BEHAVIOR_STATUS_IDLE);

    ESP_LOGI(TAG, "behavior status OK");
}

void app_behavior_status_set(behavior_status_id_t id)
{
    if (!s_inited) return;

    /* 相同状态不重复执行（避免重复触发动画/音频） */
    if (s_fsm.current == id) return;

    /* 若正处于某 scene 播放中，同步刷新"进入 scene 前的状态"，
     * 使 scene 结束后回到切换后的最新长期状态 */
    if (s_fsm.has_before_scene) {
        s_fsm.before_scene = id;
    }
    s_fsm.current = id;
    play_status(id);
}

void app_behavior_status_push(behavior_status_id_t id)
{
    if (!s_inited) return;

    /* 相同状态不重复入栈 */
    if (s_fsm.current == id) return;

    if (s_fsm.depth < BEHAVIOR_STATUS_STACK_DEPTH) {
        s_fsm.stack[s_fsm.depth++] = s_fsm.current;
    } else {
        ESP_LOGW(TAG, "status stack full, drop '%s'",
                 app_behavior_status_name(s_fsm.current));
    }
    /* 若正处于某 scene 播放中，同步刷新"进入 scene 前的状态" */
    if (s_fsm.has_before_scene) {
        s_fsm.before_scene = id;
    }
    s_fsm.current = id;
    play_status(id);
}

void app_behavior_status_pop(void)
{
    if (!s_inited) return;
    if (s_fsm.depth > 0) {
        s_fsm.depth--;
        s_fsm.current = s_fsm.stack[s_fsm.depth];
        play_status(s_fsm.current);
    } else {
        s_fsm.current = BEHAVIOR_STATUS_IDLE;
        play_status(BEHAVIOR_STATUS_IDLE);
    }
}

behavior_status_id_t app_behavior_status_current(void)
{
    return s_inited ? s_fsm.current : BEHAVIOR_STATUS_IDLE;
}

const char *app_behavior_status_current_name(void)
{
    return app_behavior_status_name(app_behavior_status_current());
}

void app_behavior_status_set_name(const char *name)
{
    if (!name || !name[0]) return;
    behavior_status_id_t id;
    if (!app_behavior_status_from_name(name, &id)) {
        ESP_LOGW(TAG, "preload_assets unknown status '%s', ignored", name);
        return;
    }
    app_behavior_status_set(id);   /* 内部已对相同状态去重 */
}
