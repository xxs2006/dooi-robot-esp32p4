#include "app_behavior.h"
#include "app_behavior_fs.h"
#include "app_behavior_status.h"
#include "behavior_xx_parse.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include <cJSON.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "app_behavior";

#define BEHAVIOR_TASK_STACK    4096
#define BEHAVIOR_TASK_PRIO     7
#define TICK_INTERVAL_MS       20
#define BEHAVIOR_SCENE_MAX_MS  30000   /* 场景最长 30s，超过强制收尾（防 repeat=0 疏漏导致永不恢复）；STATUS 类不受此限 */

/* ==================== 消息 ==================== */
typedef enum { BMSG_PLAY = 0, BMSG_STOP } bmsg_type_t;

typedef struct {
    bmsg_type_t type;
    bool             has_pb;      /* true: pb 直接携带；false: name+is_status 懒加载 */
    behavior_playback_t *pb;
    char             name[BEHAVIOR_NAME_MAX];
    bool             is_status;
    action_priority_t drop_below;
} bmsg_t;

/* ==================== 管理器状态 ==================== */
typedef struct {
    QueueHandle_t queue;
    TaskHandle_t  task;
    behavior_playback_t *current;
    behavior_playback_t *pending[BEHAVIOR_QUEUE_DEPTH];
    uint8_t        pending_count;
} bman_t;

static bman_t s_bman;
static volatile bool s_idle = true;
static behavior_scene_ended_hook_t s_scene_ended_hook = NULL;
static behavior_scene_started_hook_t s_scene_started_hook = NULL;

/* ==================== 工具 ==================== */
static inline uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

/*
 * 把 cJSON 的内存分配重定向到 PSRAM。
 *
 * 项目此前从未调用 cJSON_InitHooks，所有 cJSON 树走默认 malloc（内建 RAM）。
 * P4 有 32MB PSRAM（HEX/200M/XIP），且 cJSON 结构体从不参与 DMA，
 * 改走 PSRAM 既安全又释放内建 RAM。安装一次后全局生效。
 */
static void *cjson_psram_malloc(size_t sz)
{
    return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void cjson_psram_free(void *p)
{
    heap_caps_free(p);
}

static void app_behavior_cjson_install_hooks(void)
{
    cJSON_Hooks hooks = {
        .malloc_fn = cjson_psram_malloc,
        .free_fn   = cjson_psram_free,
    };
    cJSON_InitHooks(&hooks);
    ESP_LOGI(TAG, "cJSON hooks -> PSRAM");
}

static bool prio_from_str(const char *s, action_priority_t *out)
{
    if (!s || !out) return false;
    static const struct { const char *n; action_priority_t p; } m[] = {
        {"idle", PRIO_IDLE}, {"low", PRIO_LOW}, {"normal", PRIO_NORMAL},
        {"high", PRIO_HIGH}, {"critical", PRIO_CRITICAL},
    };
    for (size_t i = 0; i < sizeof(m) / sizeof(m[0]); i++) {
        if (strcmp(s, m[i].n) == 0) { *out = m[i].p; return true; }
    }
    return false;
}

/* ==================== PSRAM 驻留解析缓存 ====================
 *
 * 调用动作的高延迟主要来自 playback_load() 在调用方上下文反复做：
 *   scene JSON 解析 -> asset JSON 解析 -> channel->parse()。
 * 这些解析结果本质是"不可变配置"，且本模块要求"优先 PSRAM、可驻留、无需释放"。
 * 因此这里做两级缓存，全部常驻 PSRAM，播放/结束绝不对缓存对象调用 free/destroy：
 *
 *  L1  asset 解析缓存：按 "<channel>/<asset>" 缓存 channel->parse() 的 play_data。
 *      同一资产在多个场景/多次播放间共享，只解析一次。
 *  L2  scene 预设解析缓存：按 "<scene>|<preset>" 缓存已解析的时间轴配置，
 *      命中时零 JSON、零 parse，直接深拷贝轻量配置重建播放体。
 */
/* 资源数量在编译期固定（~132 个 asset、54 场景 + 3 状态，场景含多 preset 组合），
 * 容量取足够大即可全部驻留，且 web 临时场景也会入库。 */
#define BEHAVIOR_CACHE_ASSET_CAP  160
#define BEHAVIOR_CACHE_SCENE_CAP  192
#define BEHAVIOR_CACHE_KEY_MAX    64

/* L1：asset 解析缓存项 */
typedef struct {
    char     key[BEHAVIOR_CACHE_KEY_MAX];   /* "<channel>/<asset>" */
    const behavior_channel_t *ch;
    void    *play_data;                     /* 驻留，不可 free */
} asset_cache_t;

/* L2 场景缓存的时间轴条目（轻量配置，play_data 借用 L1 驻留指针） */
typedef struct {
    const behavior_channel_t *ch;
    void    *play_data;
    uint32_t time_ms;
    uint8_t  repeat;
    bool     has_infinite;
    char     asset[BEHAVIOR_ASSET_MAX];
} sc_item_t;

/* L2：scene 预设解析缓存项 */
typedef struct {
    char     key[BEHAVIOR_CACHE_KEY_MAX];   /* "<scene>|<preset>" */
    behavior_kind_t   kind;
    action_priority_t priority;
    char     preset[BEHAVIOR_NAME_MAX];     /* 实际选中的 preset 名 */
    bool     has_restore;
    char     restore_status[BEHAVIOR_NAME_MAX];
    bool     has_preload;
    bool     no_restore;
    char     preload_assets[BEHAVIOR_NAME_MAX];
    uint8_t  item_count;
    sc_item_t items[BEHAVIOR_TIMELINE_MAX]; /* 时间轴配置 */
} scene_cache_t;

/* 缓存表动态分配于 PSRAM 堆，避免占用内部 DRAM（内容"驻留、无需释放"） */
static asset_cache_t *s_asset_cache = NULL;
static uint8_t        s_asset_cache_count = 0;
static scene_cache_t *s_scene_cache = NULL;
static uint8_t        s_scene_cache_count = 0;

/* playback_load 会被多个任务并发调用（web / mcp / xbox / behavior 任务懒加载），
 * 而它读写共享的 L1/L2 缓存，故需互斥保护，避免缓存表数据竞争。 */
static SemaphoreHandle_t s_cache_lock = NULL;
static void cache_lock(void)   { if (s_cache_lock) xSemaphoreTakeRecursive(s_cache_lock, portMAX_DELAY); }
static void cache_unlock(void) { if (s_cache_lock) xSemaphoreGiveRecursive(s_cache_lock); }

/* 通用 PSRAM 分配/释放助手（返回 NULL 表示失败；释放可传 NULL） */
static void *bhv_malloc(size_t sz)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(sz);   /* 兜底内建 RAM，保证不因 PSRAM 不足而失败 */
}
static void *bhv_calloc(size_t n, size_t sz)
{
    void *p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(n, sz);
}
static void bhv_free(void *p)
{
    if (p) heap_caps_free(p);
}

/* L1：按 "<channel>/<asset>" 取驻留 play_data；未命中返回 NULL */
static void *asset_cache_get(const char *key)
{
    if (!s_asset_cache) return NULL;
    for (uint8_t i = 0; i < s_asset_cache_count; i++) {
        if (strcmp(s_asset_cache[i].key, key) == 0) return s_asset_cache[i].play_data;
    }
    return NULL;
}

/* L1：存入（仅插入，永不替换/释放；满则忽略，下次仍会走一次完整解析但可接受） */
static void asset_cache_put(const char *key, const behavior_channel_t *ch, void *play_data)
{
    if (!s_asset_cache || s_asset_cache_count >= BEHAVIOR_CACHE_ASSET_CAP) return;
    asset_cache_t *e = &s_asset_cache[s_asset_cache_count];
    snprintf(e->key, sizeof(e->key), "%s", key);
    e->ch = ch;
    e->play_data = play_data;
    s_asset_cache_count++;
}

/* L2：取 scene 预设解析缓存，命中返回指针（只读） */
static const scene_cache_t *scene_cache_get(const char *key)
{
    if (!s_scene_cache) return NULL;
    for (uint8_t i = 0; i < s_scene_cache_count; i++) {
        if (strcmp(s_scene_cache[i].key, key) == 0) return &s_scene_cache[i];
    }
    return NULL;
}

/* L2：存入（幂等：已存在同 key 则跳过，避免随机 preset 重复选中的重复项） */
static void scene_cache_put(const scene_cache_t *src)
{
    if (!s_scene_cache) return;
    for (uint8_t i = 0; i < s_scene_cache_count; i++) {
        if (strcmp(s_scene_cache[i].key, src->key) == 0) return;   /* 已有，跳过 */
    }
    if (s_scene_cache_count >= BEHAVIOR_CACHE_SCENE_CAP) return;
    scene_cache_t *e = &s_scene_cache[s_scene_cache_count];
    memcpy(e, src, sizeof(*e));   /* 轻量值拷贝，play_data 为借用指针 */
    s_scene_cache_count++;
}

/* ==================== 缓存失效（资源热更新） ====================
 *
 * L1/L2 的内容全部派生自 /resources 下的 JSON。网页编辑器保存文件后只刷新了
 * 文本缓存（app_behavior_fs），若不同步失效这里的【解析结果】，播放时仍会
 * 命中旧 play_data / 旧时间轴 —— 这正是"网页保存后执行仍是旧数据"的根因。
 *
 * 难点：play_data 是"驻留共享"的，正在播放 / 排队中的播放体直接借用该指针，
 * 因此失效时不能无条件 destroy。这里引入"待回收(stale)"队列：
 *   - 若该 play_data 仍被某个存活播放体引用 -> 挂入 stale 队列延后回收；
 *   - 否则立即 destroy。
 * stale 队列在每次失效调用与行为任务 tick（约 1s 一次）中尝试回收。
 */
#define BEHAVIOR_STALE_CAP   32

typedef struct {
    const behavior_channel_t *ch;
    void                     *play_data;
} stale_entry_t;

static stale_entry_t s_stale[BEHAVIOR_STALE_CAP];
static uint8_t       s_stale_count = 0;

/* 该 play_data 是否仍被存活播放体（正在播放 / 排队中）借用。
 * 只做指针比较。与行为任务对 s_bman 的并发改动属良性竞态：
 *   读到旧指针 => 判定"在用" => 延后回收（安全）；
 *   读到 NULL  => 该条目已被 playback_free 释放，不再引用（安全）。
 * 调用方须持 cache_lock（保证此刻无并发的 asset_resolve/parse）。 */
static bool play_data_in_use(const void *pd)
{
    if (!pd) return false;

    const behavior_playback_t *list[BEHAVIOR_QUEUE_DEPTH + 1];
    size_t n = 0;
    if (s_bman.current) list[n++] = s_bman.current;
    for (uint8_t i = 0; i < s_bman.pending_count && n < (sizeof(list) / sizeof(list[0])); i++) {
        list[n++] = s_bman.pending[i];
    }

    for (size_t k = 0; k < n; k++) {
        const behavior_playback_t *pb = list[k];
        if (!pb) continue;
        for (uint8_t i = 0; i < pb->item_count; i++) {
            if (pb->items[i].play_data == pd) return true;
        }
    }
    return false;
}

/* 挂入待回收队列（队列满时兜底回收最旧项，防泄漏）。调用方须持 cache_lock。 */
static void stale_push(const behavior_channel_t *ch, void *pd)
{
    if (!ch || !pd) return;
    if (s_stale_count >= BEHAVIOR_STALE_CAP) {
        ESP_LOGW(TAG, "stale list full, force reclaim oldest");
        s_stale[0].ch->destroy(s_stale[0].play_data, false);
        memmove(&s_stale[0], &s_stale[1], (BEHAVIOR_STALE_CAP - 1) * sizeof(s_stale[0]));
        s_stale_count--;
    }
    s_stale[s_stale_count].ch        = ch;
    s_stale[s_stale_count].play_data = pd;
    s_stale_count++;
}

/* 回收 stale 队列中已无引用的 play_data。调用方须持 cache_lock。 */
static void stale_drain(void)
{
    for (uint8_t i = 0; i < s_stale_count; ) {
        if (play_data_in_use(s_stale[i].play_data)) { i++; continue; }
        s_stale[i].ch->destroy(s_stale[i].play_data, false);
        s_stale[i] = s_stale[s_stale_count - 1];   /* swap-pop */
        s_stale_count--;
    }
}

/* L1：按 key 失效一项（swap-pop 移除，并尽量回收其 play_data）。
 * 调用方须持 cache_lock。 */
static void asset_cache_invalidate(const char *key)
{
    if (!s_asset_cache || !key) return;
    for (uint8_t i = 0; i < s_asset_cache_count; i++) {
        if (strcmp(s_asset_cache[i].key, key) != 0) continue;

        const behavior_channel_t *ch = s_asset_cache[i].ch;
        void *pd = s_asset_cache[i].play_data;
        s_asset_cache[i] = s_asset_cache[s_asset_cache_count - 1];   /* swap-pop */
        s_asset_cache_count--;

        if (pd) {
            if (play_data_in_use(pd)) stale_push(ch, pd);   /* 仍在播放，延后回收 */
            else ch->destroy(pd, false);
        }
        return;
    }
}

/* L2：移除 key 完全匹配的条目（值拷贝，仅借用指针，不释放任何 play_data）。
 * 调用方须持 cache_lock。 */
static void scene_cache_invalidate(const char *key)
{
    if (!s_scene_cache || !key) return;
    for (uint8_t i = 0; i < s_scene_cache_count; i++) {
        if (strcmp(s_scene_cache[i].key, key) == 0) {
            s_scene_cache[i] = s_scene_cache[s_scene_cache_count - 1];   /* swap-pop */
            s_scene_cache_count--;
            return;
        }
    }
}

/* L2：移除所有"引用了指定 asset"的条目 —— 该 asset 的 play_data 即将失效，
 * 残留条目会变成悬垂指针。调用方须持 cache_lock。 */
static void scene_cache_invalidate_asset(const behavior_channel_t *ch, const char *asset)
{
    if (!s_scene_cache || !ch || !asset) return;
    for (uint8_t i = 0; i < s_scene_cache_count; ) {
        bool hit = false;
        for (uint8_t j = 0; j < s_scene_cache[i].item_count; j++) {
            const sc_item_t *e = &s_scene_cache[i].items[j];
            if (e->ch == ch && strcmp(e->asset, asset) == 0) { hit = true; break; }
        }
        if (hit) {
            s_scene_cache[i] = s_scene_cache[s_scene_cache_count - 1];   /* swap-pop */
            s_scene_cache_count--;
        } else {
            i++;
        }
    }
}

/* L2：移除某场景的所有 preset 条目（键形如 "<scene>|<preset>"）。
 * 调用方须持 cache_lock。 */
static void scene_cache_invalidate_scene(const char *scene)
{
    if (!s_scene_cache || !scene || !scene[0]) return;
    size_t n = strlen(scene);
    for (uint8_t i = 0; i < s_scene_cache_count; ) {
        const char *k = s_scene_cache[i].key;
        if (strncmp(k, scene, n) == 0 && k[n] == '|') {
            s_scene_cache[i] = s_scene_cache[s_scene_cache_count - 1];   /* swap-pop */
            s_scene_cache_count--;
        } else {
            i++;
        }
    }
}

/* 从 "a/b/c.json" 取文件名主干 "c"；空名或非法入参返回 false。
 * 超长时截断到 outsz-1（而非失败）：播放体名与 L2 键都有长度上限，
 * 且所有调用方（播放 / 失效）用同一上限截断，键仍保持一致。 */
static bool name_from_rel_path(const char *rel_path, char *out, size_t outsz)
{
    if (!rel_path || !rel_path[0] || !out || outsz == 0) return false;
    const char *slash = strrchr(rel_path, '/');
    const char *fname = slash ? (slash + 1) : rel_path;
    const char *dot   = strrchr(fname, '.');
    size_t n = dot ? (size_t)(dot - fname) : strlen(fname);
    if (n == 0) return false;
    if (n >= outsz) n = outsz - 1;
    memcpy(out, fname, n);
    out[n] = '\0';
    return true;
}

/* ==================== 时间轴推进 ==================== */
static void item_maybe_start(behavior_timeline_t *it, uint64_t now, uint64_t bhv_start)
{
    if (it->started || it->done || !it->play_data) return;
    if (it->channel->keyframe_count(it->play_data) <= 0) { it->done = true; return; }
    if (now < bhv_start + it->time_ms) return;

    it->started = true;
    it->start_wall_ms = bhv_start + it->time_ms;
    it->last_pass = 0;
    it->last_idx = 0;
}

static void item_advance(behavior_timeline_t *it, uint64_t now)
{
    if (it->done || !it->started || !it->play_data) return;
    const behavior_channel_t *ch = it->channel;
    int n = ch->keyframe_count(it->play_data);
    if (n <= 0) { it->done = true; return; }

    uint64_t dur = ch->duration_ms(it->play_data);
    if (dur == 0) dur = 1;
    uint64_t el    = now - it->start_wall_ms;
    uint64_t pass  = el / dur;
    uint64_t local = el % dur;

    /* 有限次数：播完即止 */
    if (it->repeat != 0 && pass >= it->repeat) { it->done = true; return; }

    /* 进入新 pass：重置 keyframe 游标 */
    if ((uint32_t)pass != it->last_pass) {
        it->last_pass = (uint32_t)pass;
        it->last_idx = 0;
    }

    int idx = it->last_idx;
    while (idx < n && ch->kf_time_ms(it->play_data, idx) <= local) idx++;
    for (int i = it->last_idx; i < idx; i++) ch->apply(it->play_data, i);
    it->last_idx = idx;
}

static void step_playback(behavior_playback_t *pb, uint64_t now)
{
    for (uint8_t i = 0; i < pb->item_count; i++) {
        item_maybe_start(&pb->items[i], now, pb->start_wall_ms);
        item_advance(&pb->items[i], now);
    }
}

static bool all_items_done(const behavior_playback_t *pb)
{
    for (uint8_t i = 0; i < pb->item_count; i++)
        if (!pb->items[i].done) return false;
    return true;
}

/* ==================== 播放体生命周期 ==================== */
/* 释放播放体。play_data 绝大多数来自 PSRAM 驻留缓存（owned_play==false，共享、不可 free），
 * 这里只释放播放体外壳本身。仅当 owned_play==true（缓存满回退的自有分配）时才 destroy 回收。 */
static void playback_free(behavior_playback_t *pb)
{
    if (!pb) return;
    for (uint8_t i = 0; i < pb->item_count; i++) {
        behavior_timeline_t *it = &pb->items[i];
        if (it->owned_play) {
            if (it->play_data) {
                it->channel->destroy(it->play_data, false);
                it->play_data = NULL;
            }
        } else {
            it->play_data = NULL;   /* 共享驻留，不释放 */
        }
        if (it->asset_json) { cJSON_Delete(it->asset_json); it->asset_json = NULL; }
    }
    bhv_free(pb);
}

/* ==================== 加载 ==================== */
static const cJSON *pick_preset(const cJSON *presets, const char *want)
{
    if (!cJSON_IsArray(presets)) return NULL;
    int n = cJSON_GetArraySize(presets);
    if (n <= 0) return NULL;

    if (want && want[0]) {
        for (int i = 0; i < n; i++) {
            const cJSON *v = cJSON_GetArrayItem(presets, i);
            const cJSON *vn = cJSON_GetObjectItemCaseSensitive(v, "name");
            if (vn && cJSON_IsString(vn) && vn->valuestring &&
                strcmp(vn->valuestring, want) == 0) return v;
        }
        return NULL;
    }

    long total = 0;
    for (int i = 0; i < n; i++) {
        const cJSON *v = cJSON_GetArrayItem(presets, i);
        const cJSON *w = cJSON_GetObjectItemCaseSensitive(v, "weight");
        total += (w && cJSON_IsNumber(w)) ? (long)w->valueint : 1L;
    }
    if (total <= 0) return cJSON_GetArrayItem(presets, 0);

    long r = (long)(esp_random() % (uint32_t)total);
    long acc = 0;
    for (int i = 0; i < n; i++) {
        const cJSON *v = cJSON_GetArrayItem(presets, i);
        const cJSON *w = cJSON_GetObjectItemCaseSensitive(v, "weight");
        acc += (w && cJSON_IsNumber(w)) ? (long)w->valueint : 1L;
        if (r < acc) return v;
    }
    return cJSON_GetArrayItem(presets, n - 1);
}

/* 解析 asset 并尽量写入 L1 驻留缓存。
 * 返回 play_data（成功）；*owned 指示调用方是否"自有"：
 *   *owned=false：来自驻留缓存，共享、不可 free；
 *   *owned=true ：缓存已满回退到自有分配，调用方负责用 destroy 释放。
 * 命中缓存时零 JSON、零 parse。 */
static void *asset_resolve(const behavior_channel_t *ch, const char *chname,
                           const char *asset_name, bool *owned)
{
    *owned = false;
    char key[BEHAVIOR_CACHE_KEY_MAX];
    snprintf(key, sizeof(key), "%s/%s", chname, asset_name);

    void *pd = asset_cache_get(key);
    if (pd) return pd;                              /* 缓存命中 */

    char apath[128];
    app_behavior_fs_asset_path(apath, sizeof(apath), chname, asset_name);
    cJSON *aj = app_behavior_fs_load_json(apath);
    if (!aj) {
        ESP_LOGW(TAG, "asset '%s' missing", apath);
        return NULL;
    }
    pd = ch->parse(aj);
    cJSON_Delete(aj);                               /* play_data 为深拷贝，解析完即可释放 JSON */
    if (!pd) {
        ESP_LOGW(TAG, "asset '%s' unparseable for '%s'", asset_name, chname);
        return NULL;
    }
    if (s_asset_cache_count < BEHAVIOR_CACHE_ASSET_CAP) {
        asset_cache_put(key, ch, pd);               /* 驻留，永不释放，返回借用 */
    } else {
        *owned = true;                              /* 缓存满，调用方自有，须 destroy */
    }
    return pd;
}

/* 把场景/状态 JSON 的时间轴条目填充进 playback 或 scene_cache（冷路径共用）。
 * sc!=NULL 时写入 L2 场景缓存配置项；否则写入播放体条目。 */
static void load_timeline_item(behavior_playback_t *pb, const cJSON *ti,
                               scene_cache_t *sc)
{
    const char *chname = NULL;
    if (!behavior_parse_channel_name(ti, &chname)) {
        ESP_LOGW(TAG, "'%s': item missing channel", pb ? pb->name : "");
        return;
    }
    const behavior_channel_t *ch = behavior_channel_find(chname);
    if (!ch) {
        ESP_LOGW(TAG, "'%s': unknown channel '%s'", pb ? pb->name : "", chname);
        return;
    }
    const cJSON *asset = cJSON_GetObjectItemCaseSensitive(ti, "asset");
    if (!cJSON_IsString(asset) || !asset->valuestring) {
        ESP_LOGW(TAG, "'%s': item missing asset", pb ? pb->name : "");
        return;
    }

    uint32_t time_ms = 0;
    behavior_kf_get_u32(ti, "time_ms", &time_ms);
    uint32_t repeat = 1;
    const cJSON *rp = cJSON_GetObjectItemCaseSensitive(ti, "repeat");
    if (rp && cJSON_IsNumber(rp) && rp->valueint >= 0) repeat = (uint32_t)rp->valueint;

    bool owned = false;
    void *pd = asset_resolve(ch, chname, asset->valuestring, &owned);
    if (!pd) return;

    if (sc) {
        /* 写入 L2 场景缓存：只接受驻留(借用) play_data；缓存满的自有数据不入缓存 */
        if (owned) {
            ch->destroy(pd, false);
            return;
        }
        if (sc->item_count >= BEHAVIOR_TIMELINE_MAX) return;
        sc_item_t *e = &sc->items[sc->item_count];
        e->ch = ch;
        e->play_data = pd;
        e->time_ms = time_ms;
        e->repeat = (uint8_t)(repeat > 0xFF ? 0xFF : repeat);
        e->has_infinite = (repeat == 0);
        snprintf(e->asset, sizeof(e->asset), "%s", asset->valuestring);
        sc->item_count++;
        return;
    }

    behavior_timeline_t *it = &pb->items[pb->item_count];
    memset(it, 0, sizeof(*it));
    it->channel = ch;
    it->time_ms = time_ms;
    it->repeat = (uint8_t)(repeat > 0xFF ? 0xFF : repeat);
    snprintf(it->asset, sizeof(it->asset), "%s", asset->valuestring);
    it->play_data = pd;
    it->owned_play = owned;                         /* true=自有(缓存满)；false=借用驻留 */
    pb->item_count++;
    if (it->repeat == 0) pb->has_infinite = true;
}

/* 冷加载：完整解析场景 JSON + 各 asset，并填入 L2 scene 缓存。
 * 返回后调用方应 cJSON_Delete(root)。返回 false 表示无法构成可用播放体。 */
static bool scene_config_load(scene_cache_t *sc, const char *path, const char *name,
                              behavior_kind_t kind, const char *preset)
{
    cJSON *root = app_behavior_fs_load_json(path);
    if (!root) {
        ESP_LOGW(TAG, "load '%s' failed at %s", name, path);
        return false;
    }

    if (kind == BEHAVIOR_KIND_STATUS) {
        sc->priority = PRIO_IDLE;
    } else {
        sc->priority = PRIO_NORMAL;
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "priority");
        action_priority_t pp;
        if (p && cJSON_IsString(p) && prio_from_str(p->valuestring, &pp)) sc->priority = pp;

        const cJSON *config = cJSON_GetObjectItemCaseSensitive(root, "config");
        const cJSON *restore = config ? cJSON_GetObjectItemCaseSensitive(config, "restore_status") : NULL;
        if (restore && cJSON_IsString(restore) && restore->valuestring) {
            snprintf(sc->restore_status, sizeof(sc->restore_status), "%s", restore->valuestring);
            sc->has_restore = true;
        }

        /* 瞬态/纯音效场景：播放结束不触发"恢复原状态"，避免反复打断-恢复基线状态 */
        const cJSON *no_restore = config ? cJSON_GetObjectItemCaseSensitive(config, "no_restore") : NULL;
        if (no_restore && cJSON_IsTrue(no_restore)) sc->no_restore = true;

        const cJSON *preload = config ? cJSON_GetObjectItemCaseSensitive(config, "preload_assets") : NULL;
        if (!preload || !cJSON_IsString(preload) || !preload->valuestring) {
            preload = cJSON_GetObjectItemCaseSensitive(root, "preload_assets");
        }
        if (preload && cJSON_IsString(preload) && preload->valuestring) {
            const char *pv = preload->valuestring;
            size_t n = strlen(pv);
            if (n > 5 && strcmp(pv + n - 5, ".json") == 0) n -= 5;
            if (n >= sizeof(sc->preload_assets)) n = sizeof(sc->preload_assets) - 1;
            memcpy(sc->preload_assets, pv, n);
            sc->preload_assets[n] = '\0';
            sc->has_preload = true;
        }
    }

    const cJSON *presets = cJSON_GetObjectItemCaseSensitive(root, "presets");
    const cJSON *chosen = pick_preset(presets, preset);
    if (chosen) {
        const cJSON *vname = cJSON_GetObjectItemCaseSensitive(chosen, "name");
        if (vname && cJSON_IsString(vname) && vname->valuestring)
            snprintf(sc->preset, sizeof(sc->preset), "%s", vname->valuestring);
        const cJSON *timeline = cJSON_GetObjectItemCaseSensitive(chosen, "timeline");
        if (cJSON_IsArray(timeline)) {
            int n = cJSON_GetArraySize(timeline);
            for (int i = 0; i < n; i++) {
                load_timeline_item(NULL, cJSON_GetArrayItem(timeline, i), sc);
            }
        }
    } else {
        ESP_LOGW(TAG, "'%s' has no usable preset", name);
    }

    cJSON_Delete(root);
    return sc->item_count > 0;
}

/* 从已解析的 scene 配置重建一个播放体（深拷贝轻量字段，零 JSON/零 parse）。
 * 各 item 的 play_data 借用驻留缓存指针，仅复制配置 + 复位运行时字段。 */
static behavior_playback_t *playback_from_cache(const scene_cache_t *sc,
                                                const char *name, const char *preset,
                                                action_priority_t prio_override)
{
    behavior_playback_t *pb = (behavior_playback_t *)bhv_calloc(1, sizeof(*pb));
    if (!pb) return NULL;
    snprintf(pb->name, sizeof(pb->name), "%s", name);
    pb->kind = sc->kind;
    pb->priority = sc->priority;
    if (prio_override != BEHAVIOR_PRIO_DEFAULT) pb->priority = prio_override;
    pb->no_restore = sc->no_restore;
    if (sc->has_restore) {
        snprintf(pb->restore_status, sizeof(pb->restore_status), "%s", sc->restore_status);
        pb->has_restore = true;
    }
    if (sc->has_preload) {
        snprintf(pb->preload_assets, sizeof(pb->preload_assets), "%s", sc->preload_assets);
        pb->has_preload = true;
    }
    for (uint8_t i = 0; i < sc->item_count; i++) {
        behavior_timeline_t *it = &pb->items[i];
        memset(it, 0, sizeof(*it));
        it->channel    = sc->items[i].ch;
        it->time_ms    = sc->items[i].time_ms;
        it->repeat     = sc->items[i].repeat;
        it->play_data  = sc->items[i].play_data;   /* 借用驻留缓存 */
        it->owned_play = false;
        snprintf(it->asset, sizeof(it->asset), "%s", sc->items[i].asset);
        if (sc->items[i].has_infinite) pb->has_infinite = true;
    }
    pb->item_count = sc->item_count;
    if (preset && preset[0]) {
        snprintf(pb->preset, sizeof(pb->preset), "%s", preset);
    } else if (sc->preset[0]) {
        snprintf(pb->preset, sizeof(pb->preset), "%s", sc->preset);
    }
    return pb;
}

/* rel_path 非空时直接用它作为资源路径（网页编辑器按完整路径播放），
 * 否则按 name + kind 走 app_behavior_fs 的路径猜测（兼容既有调用方）。 */
static behavior_playback_t *playback_load_ex(const char *rel_path, const char *name,
                                             behavior_kind_t kind, const char *preset,
                                             action_priority_t prio_override)
{
    if (!name || !name[0]) return NULL;

    cache_lock();

    /* L2 缓存键：status 用 "status/<name>"；scene 用 "<name>|<preset>"。
     * 若 preset 未指定（随机），仍按实际选中的 preset 名缓存，从而保留随机化：
     * 每次带 NULL preset 的调用会因空键未命中而重新随机 pick_preset，
     * 但被选中过的那份 preset 配置会驻留，后续按名命中即可零解析。 */
    char cache_key[BEHAVIOR_CACHE_KEY_MAX];
    if (kind == BEHAVIOR_KIND_STATUS) {
        snprintf(cache_key, sizeof(cache_key), "status/%s", name);
    } else if (preset && preset[0]) {
        snprintf(cache_key, sizeof(cache_key), "%s|%s", name, preset);
    } else {
        /* 先探测：该场景是否已有"按名缓存"可命中。
         * 这里无法在解析前知道随机结果，故走一次完整解析（asset 由 L1 兜底）。 */
        cache_key[0] = '\0';
    }

    if (cache_key[0]) {
        const scene_cache_t *hit = scene_cache_get(cache_key);
        if (hit) {
            behavior_playback_t *pb = playback_from_cache(hit, name, preset, prio_override);
            if (pb && pb->item_count > 0) { cache_unlock(); return pb; }
            bhv_free(pb);
        }
    }

    char path[96];
    if (rel_path && rel_path[0]) {
        snprintf(path, sizeof(path), "%s", rel_path);
    } else if (kind == BEHAVIOR_KIND_STATUS) {
        app_behavior_fs_status_path(path, sizeof(path), name);
    } else if (app_behavior_fs_scene_path(path, sizeof(path), name) == 0) {
        /* 场景路径探测失败时 path 内容未定义，必须在此拦住，
         * 否则会把上一轮的残留字符串当路径去加载。 */
        ESP_LOGW(TAG, "scene '%s' not found", name);
        cache_unlock();
        return NULL;
    }

    scene_cache_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.kind = kind;
    if (!scene_config_load(&sc, path, name, kind, preset)) {
        ESP_LOGW(TAG, "'%s' has no valid timeline items", name);
        cache_unlock();
        return NULL;
    }

    /* 用实际选中 preset 名作为缓存键，缓存驻留 */
    if (kind == BEHAVIOR_KIND_STATUS) {
        snprintf(cache_key, sizeof(cache_key), "status/%s", name);
    } else {
        snprintf(cache_key, sizeof(cache_key), "%s|%s", name,
                 sc.preset[0] ? sc.preset : (preset ? preset : ""));
    }
    snprintf(sc.key, sizeof(sc.key), "%s", cache_key);
    scene_cache_put(&sc);

    behavior_playback_t *pb = playback_from_cache(&sc, name, preset, prio_override);
    cache_unlock();
    return pb;
}

static behavior_playback_t *playback_load(const char *name, behavior_kind_t kind,
                                          const char *preset, action_priority_t prio_override)
{
    return playback_load_ex(NULL, name, kind, preset, prio_override);
}

/* ==================== 运行时推进 ==================== */
static void finish_current(bool interrupted);
static void start_playback(behavior_playback_t *pb);

static void handle_play(behavior_playback_t *pb)
{
    if (s_bman.current == NULL) {
        start_playback(pb);
        return;
    }
    /* 高优先级（或同优先级的新请求）打断当前：
     * 同优先级用 >= 而非 >，避免相同优先级的连续交互请求互相排队，
     * 否则后一个请求必须等前一个播完才执行，体感延迟随场景时长放大。 */
    if (pb->priority >= s_bman.current->priority) {
        finish_current(true);
        start_playback(pb);
        return;
    }
    /* 否则入队（同优先级 FIFO，后置高优先级之前） */
    if (s_bman.pending_count >= BEHAVIOR_QUEUE_DEPTH) {
        ESP_LOGW(TAG, "queue full, drop '%s'", pb->name);
        playback_free(pb);
        return;
    }
    int pos = (int)s_bman.pending_count;
    while (pos > 0 && s_bman.pending[pos - 1]->priority < pb->priority) pos--;
    memmove(&s_bman.pending[pos + 1], &s_bman.pending[pos],
            (s_bman.pending_count - (size_t)pos) * sizeof(behavior_playback_t *));
    s_bman.pending[pos] = pb;
    s_bman.pending_count++;
}

static void handle_stop(action_priority_t drop_below)
{
    if (s_bman.current && s_bman.current->priority >= drop_below) {
        finish_current(true);
    }
    int i = (int)s_bman.pending_count - 1;
    while (i >= 0) {
        if (s_bman.pending[i]->priority >= drop_below) {
            playback_free(s_bman.pending[i]);
            memmove(&s_bman.pending[i], &s_bman.pending[i + 1],
                    (s_bman.pending_count - (size_t)i - 1) * sizeof(behavior_playback_t *));
            s_bman.pending_count--;
        } else {
            i--;
        }
    }
}

static void start_playback(behavior_playback_t *pb)
{
    /* 场景前置基线：先确保 preload_assets 所声明的状态已激活。
     * 若当前已是该状态则跳过，避免重复触发动画/音频带来的延迟。
     * [临时关闭] preload_assets 功能暂不可用 */
    if (false && pb->kind == BEHAVIOR_KIND_SCENE && pb->has_preload) {
        const char *cur = app_behavior_status_current_name();
        if (strcmp(cur, pb->preload_assets) == 0) {
            ESP_LOGI(TAG, "scene '%s': preload '%s' already active, skip",
                     pb->name, pb->preload_assets);
        } else {
            ESP_LOGI(TAG, "scene '%s': preload '%s' (cur='%s')",
                     pb->name, pb->preload_assets, cur);
            app_behavior_status_set_name(pb->preload_assets);
        }
    }

    s_bman.current = pb;
    pb->start_wall_ms = now_ms();

    ESP_LOGI(TAG, "start '%s'%s%s prio=%d%s",
             pb->name, pb->preset[0] ? " preset=" : "", pb->preset[0] ? pb->preset : "",
             (int)pb->priority, pb->has_infinite ? " [infinite]" : "");

    step_playback(pb, pb->start_wall_ms);

    if (pb->kind == BEHAVIOR_KIND_SCENE && !pb->no_restore && s_scene_started_hook) {
        s_scene_started_hook(pb->name, pb->preset);
    }

    if (!pb->has_infinite && all_items_done(pb)) {
        finish_current(false);
    }
}

static void finish_current(bool interrupted)
{
    behavior_playback_t *pb = s_bman.current;
    if (!pb) return;
    s_bman.current = NULL;

    /* 复位各通道硬件（已启动的项）：
     * 统一走 stop()，只复位硬件、不释放 play_data（play_data 多为驻留共享，
     * 内存回收交由 playback_free 在 playback_free 中按 asset_json 区分处理）。 */
    for (uint8_t i = 0; i < pb->item_count; i++) {
        behavior_timeline_t *it = &pb->items[i];
        if (it->started && it->play_data) {
            it->channel->stop(it->play_data, interrupted);
        }
    }

    uint64_t elapsed = now_ms() - pb->start_wall_ms;

    behavior_result_t result;
    memset(&result, 0, sizeof(result));
    snprintf(result.name, sizeof(result.name), "%s", pb->name);
    snprintf(result.preset, sizeof(result.preset), "%s", pb->preset);
    result.kind = pb->kind;
    result.was_interrupted = interrupted;
    result.has_infinite = pb->has_infinite;
    result.elapsed_ms = (uint32_t)elapsed;

    /* 捕获恢复信息（free 前） */
    char restore[BEHAVIOR_NAME_MAX] = {0};
    const char *restore_ptr = NULL;
    if (pb->has_restore) {
        snprintf(restore, sizeof(restore), "%s", pb->restore_status);
        restore_ptr = restore;
    }
    bool is_scene = (pb->kind == BEHAVIOR_KIND_SCENE);
    bool no_restore = pb->no_restore;
    behavior_scene_ended_hook_t hook = s_scene_ended_hook;

    playback_free(pb);

    // ESP_LOGI(TAG, "end '%s'%s", result.name, interrupted ? " (interrupted)" : "");
    if (is_scene && !no_restore && hook) hook(&result, restore_ptr);
}

static void tick_step(uint64_t now)
{
    if (s_bman.current) {
        step_playback(s_bman.current, now);
        bool done = (!s_bman.current->has_infinite && all_items_done(s_bman.current));
        if (!done && s_bman.current->kind == BEHAVIOR_KIND_SCENE) {
            uint64_t elapsed = now - s_bman.current->start_wall_ms;
            if (elapsed >= BEHAVIOR_SCENE_MAX_MS) {
                ESP_LOGW(TAG, "scene '%s' timed out at %ums (>= %ums), force finish",
                         s_bman.current->name,
                         (unsigned)elapsed, (unsigned)BEHAVIOR_SCENE_MAX_MS);
                done = true;
            }
        }
        if (done) {
            finish_current(false);
        }
    }

    if (!s_bman.current && s_bman.pending_count > 0) {
        behavior_playback_t *next = s_bman.pending[0];
        memmove(&s_bman.pending[0], &s_bman.pending[1],
                (s_bman.pending_count - 1) * sizeof(behavior_playback_t *));
        s_bman.pending_count--;
        start_playback(next);
    }
}

static void handle_msg(bmsg_t *msg)
{
    if (msg->type == BMSG_PLAY) {
        if (!msg->has_pb) {
            msg->pb = playback_load(msg->name,
                                    msg->is_status ? BEHAVIOR_KIND_STATUS : BEHAVIOR_KIND_SCENE,
                                    NULL, BEHAVIOR_PRIO_DEFAULT);
            if (!msg->pb) {
                ESP_LOGW(TAG, "lazy load failed: '%s'", msg->name);
                return;
            }
            msg->has_pb = true;
        }
        handle_play(msg->pb);
    } else {
        handle_stop(msg->drop_below);
    }
}

static void behavior_task(void *arg)
{
    (void)arg;
    uint32_t ticks = 0;
    for (;;) {
        bmsg_t msg;
        if (xQueueReceive(s_bman.queue, &msg, pdMS_TO_TICKS(TICK_INTERVAL_MS)) == pdTRUE) {
            handle_msg(&msg);
        }
        uint64_t now = now_ms();
        tick_step(now);
        s_idle = (s_bman.current == NULL && s_bman.pending_count == 0);

        /* 约每 1s 回收一次"失效时仍在播放、现已无人引用"的 play_data，避免长期占用 PSRAM */
        if (++ticks >= (1000 / TICK_INTERVAL_MS)) {
            ticks = 0;
            if (s_stale_count) {
                cache_lock();
                stale_drain();
                cache_unlock();
            }
        }
    }
}

/* ==================== 入队 ==================== */
static bool enqueue_pb(behavior_playback_t *pb)
{
    if (!s_bman.queue) { playback_free(pb); return false; }
    bmsg_t msg = { .type = BMSG_PLAY, .has_pb = true, .pb = pb };
    if (xQueueSend(s_bman.queue, &msg, 0) != pdTRUE) {
        playback_free(pb);
        return false;
    }
    return true;
}

/* ==================== AI 即时编排（内联 JSON） ====================
 *
 * 与 playback_load() 冷/热路径的区别：
 *   1. 不查、不写 L1/L2 解析缓存 —— AI 每次生成的动作都是一次性的，入缓存只会挤占常驻资源；
 *   2. 时间轴条目可自带 keyframes，无需预先落盘为 asset 文件；
 *   3. play_data 一律为该播放体自有（owned_play=true），播放结束由 playback_free() destroy 回收。
 */
static void load_timeline_item_inline(behavior_playback_t *pb, const cJSON *ti, int *kf_total)
{
    const char *chname = NULL;
    if (!behavior_parse_channel_name(ti, &chname)) {
        ESP_LOGW(TAG, "[inline] item missing 'type'/'channel'");
        return;
    }
    const behavior_channel_t *ch = behavior_channel_find(chname);
    if (!ch) {
        ESP_LOGW(TAG, "[inline] unknown channel '%s'", chname);
        return;
    }
    if (pb->item_count >= BEHAVIOR_TIMELINE_MAX) {
        ESP_LOGW(TAG, "[inline] timeline full (max %d), drop '%s'", BEHAVIOR_TIMELINE_MAX, chname);
        return;
    }

    uint32_t time_ms = 0;
    behavior_kf_get_u32(ti, "time_ms", &time_ms);
    uint32_t repeat = 1;
    const cJSON *rp = cJSON_GetObjectItemCaseSensitive(ti, "repeat");
    if (rp && cJSON_IsNumber(rp) && rp->valueint >= 0) repeat = (uint32_t)rp->valueint;

    /* 三种写法：
     *   A) "asset" 为对象   -> ch->parse(asset 对象)              （自有）
     *   B) "asset" 为字符串 -> 走资源解析，借用 L1 驻留缓存        （非自有，不可释放）
     *   C) 其余             -> 把条目自身当 asset 交给 ch->parse   （自有）
     *      C 是内联关键帧的主路径；iris 等无 keyframes 的通道也走它。 */
    void *pd = NULL;
    bool  owned = true;
    const cJSON *asset_field = cJSON_GetObjectItemCaseSensitive(ti, "asset");
    if (cJSON_IsObject(asset_field)) {
        pd = ch->parse(asset_field);
    } else if (cJSON_IsString(asset_field)) {
        pd = asset_resolve(ch, chname, asset_field->valuestring, &owned);
    } else {
        pd = ch->parse(ti);
    }
    if (!pd) {
        ESP_LOGW(TAG, "[inline] channel '%s' parse failed, check its keyframe fields", chname);
        return;
    }

    behavior_timeline_t *it = &pb->items[pb->item_count];
    memset(it, 0, sizeof(*it));
    it->channel    = ch;
    it->time_ms    = time_ms;
    it->repeat     = (uint8_t)(repeat > 0xFF ? 0xFF : repeat);
    it->play_data  = pd;
    it->owned_play = owned;
    snprintf(it->asset, sizeof(it->asset), "inline");
    pb->item_count++;
    if (it->repeat == 0) pb->has_infinite = true;
    if (kf_total) *kf_total += ch->keyframe_count(pd);
}

/* ==================== 启动期预热 ==================== */
/* 遍历资源缓存中的 asset 项，全部解析进 L1 驻留缓存。
 * 这样即便第一次调用动作，asset 也无需现场解析，体感延迟进一步下降。 */
static bool warmup_asset_cb(const char *rel_path, void *arg)
{
    (void)arg;
    /* rel_path 形如 "asset/<channel>/<asset>.json" */
    if (strncmp(rel_path, "asset/", 6) != 0) return true;
    const char *chname = rel_path + 6;
    const char *slash = strchr(chname, '/');
    if (!slash) return true;
    char chbuf[16];
    size_t chlen = (size_t)(slash - chname);
    if (chlen >= sizeof(chbuf)) return true;
    memcpy(chbuf, chname, chlen);
    chbuf[chlen] = '\0';

    /* 注意：运行时 asset 名带 ".json" 后缀（场景 JSON 里 "asset":"xxx.json"），
     * L1 缓存键为 "<channel>/xxx.json"，此处必须保留后缀以命中同一缓存键。 */
    const char *fname = slash + 1;
    char asset[BEHAVIOR_ASSET_MAX];
    snprintf(asset, sizeof(asset), "%s", fname);

    const behavior_channel_t *ch = behavior_channel_find(chbuf);
    if (!ch) return true;
    bool owned = false;
    void *pd = asset_resolve(ch, chbuf, asset, &owned);
    if (owned) ch->destroy(pd, false);   /* 缓存满：本次为自有分配，立即释放避免泄漏 */
    return true;
}

/* 预热 L1 asset 缓存；失败不影响后续（懒加载兜底）。 */
static void behavior_cache_warmup(void)
{
    cache_lock();
    app_behavior_fs_foreach_cached("asset/", warmup_asset_cb, NULL);
    cache_unlock();
    ESP_LOGI(TAG, "cache warmup: %u assets, %u scenes resident",
             (unsigned)s_asset_cache_count, (unsigned)s_scene_cache_count);
}

void app_behavior_invalidate_cache(const char *rel_path)
{
    if (!s_cache_lock) return;

    cache_lock();
    stale_drain();   /* 先尝试回收上一轮遗留的失效 play_data */

    if (!rel_path || !rel_path[0]) {
        /* 全量失效：先清 L2（其 play_data 全部借用 L1），再清 L1 */
        for (uint8_t i = 0; i < s_scene_cache_count; i++) {
            memset(&s_scene_cache[i], 0, sizeof(s_scene_cache[i]));
        }
        s_scene_cache_count = 0;
        while (s_asset_cache_count > 0 && s_asset_cache) {
            asset_cache_invalidate(s_asset_cache[s_asset_cache_count - 1].key);
        }
        ESP_LOGI(TAG, "cache invalidated: all");
        cache_unlock();
        return;
    }

    if (strncmp(rel_path, "asset/", 6) == 0) {
        /* "asset/<channel>/<file>.json" 的 L1 键即 "<channel>/<file>.json" */
        const char *key = rel_path + 6;
        const char *slash = strchr(key, '/');
        if (slash) {
            char chbuf[16];
            size_t n = (size_t)(slash - key);
            if (n >= sizeof(chbuf)) n = sizeof(chbuf) - 1;
            memcpy(chbuf, key, n);
            chbuf[n] = '\0';
            const behavior_channel_t *ch = behavior_channel_find(chbuf);
            /* 必须先卸掉引用它的 L2 条目，再回收 play_data，避免悬垂指针 */
            if (ch) scene_cache_invalidate_asset(ch, slash + 1);
        }
        asset_cache_invalidate(key);
        ESP_LOGI(TAG, "cache invalidated: asset '%s'", key);
    } else if (strncmp(rel_path, "scenes/", 7) == 0) {
        char name[BEHAVIOR_NAME_MAX];
        if (name_from_rel_path(rel_path, name, sizeof(name))) {
            scene_cache_invalidate_scene(name);
            ESP_LOGI(TAG, "cache invalidated: scene '%s'", name);
        }
    } else if (strncmp(rel_path, "status/", 7) == 0) {
        char name[BEHAVIOR_NAME_MAX];
        if (name_from_rel_path(rel_path, name, sizeof(name))) {
            char key[BEHAVIOR_CACHE_KEY_MAX];
            snprintf(key, sizeof(key), "status/%s", name);
            scene_cache_invalidate(key);
            ESP_LOGI(TAG, "cache invalidated: status '%s'", name);
        }
    }

    cache_unlock();
}

/* ==================== 公开 API ==================== */
void app_behavior_init(void)
{
    if (s_bman.task) return;
    memset(&s_bman, 0, sizeof(s_bman));

    /* 0. 缓存互斥锁 + PSRAM 缓存表 */
    if (!s_cache_lock) s_cache_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_asset_cache)
        s_asset_cache = (asset_cache_t *)heap_caps_calloc(BEHAVIOR_CACHE_ASSET_CAP, sizeof(*s_asset_cache),
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_scene_cache)
        s_scene_cache = (scene_cache_t *)heap_caps_calloc(BEHAVIOR_CACHE_SCENE_CAP, sizeof(*s_scene_cache),
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_asset_cache) s_asset_cache = (asset_cache_t *)calloc(BEHAVIOR_CACHE_ASSET_CAP, sizeof(*s_asset_cache));
    if (!s_scene_cache) s_scene_cache = (scene_cache_t *)calloc(BEHAVIOR_CACHE_SCENE_CAP, sizeof(*s_scene_cache));
    /* 1. 先装 cJSON hooks，后续所有 cJSON 分配走 PSRAM */
    app_behavior_cjson_install_hooks();
    /* 2. 挂载 SPIFFS 资源分区 */
    app_behavior_fs_init();
    /* 3. 预加载全部资源 JSON 文本到 PSRAM，播放/网页读取零 flash I/O */
    app_behavior_fs_preload();
    /* 4. 预热 L1 asset 解析缓存，让首次调用也走驻留零解析 */
    behavior_cache_warmup();

    s_bman.queue = xQueueCreate(BEHAVIOR_QUEUE_DEPTH, sizeof(bmsg_t));
    configASSERT(s_bman.queue);

    BaseType_t ok = xTaskCreate(behavior_task, "behavior", BEHAVIOR_TASK_STACK,
                                NULL, BEHAVIOR_TASK_PRIO, &s_bman.task);
    configASSERT(ok == pdPASS);
    ESP_LOGI(TAG, "behavior manager OK");
}

bool app_behavior_play_scene(const char *scene_name, const char *preset,
                             action_priority_t prio_override)
{
    behavior_playback_t *pb = playback_load(scene_name, BEHAVIOR_KIND_SCENE, preset, prio_override);
    if (!pb) return false;
    return enqueue_pb(pb);
}

bool app_behavior_play_scene_path(const char *rel_path, const char *preset,
                                  action_priority_t prio_override)
{
    char name[BEHAVIOR_NAME_MAX];
    if (!name_from_rel_path(rel_path, name, sizeof(name))) return false;

    behavior_playback_t *pb = playback_load_ex(rel_path, name, BEHAVIOR_KIND_SCENE,
                                               preset, prio_override);
    if (!pb) return false;
    return enqueue_pb(pb);
}

bool app_behavior_play_scene_json(const char *scene_name, const char *json_text,
                                  action_priority_t prio_override,
                                  behavior_inline_result_t *result)
{
    if (result) memset(result, 0, sizeof(*result));

    if (!s_bman.queue) {
        if (result) snprintf(result->error, sizeof(result->error), "行为引擎未初始化（app_behavior_init 尚未完成）");
        return false;
    }
    if (!json_text || !json_text[0]) {
        if (result) snprintf(result->error, sizeof(result->error), "JSON 为空：请传入完整的编排 JSON 文本");
        return false;
    }

    size_t len = strlen(json_text);
    if (len > BEHAVIOR_INLINE_JSON_MAX) {
        if (result) snprintf(result->error, sizeof(result->error),
                             "JSON 过长：%u 字节，上限 %u 字节，请精简关键帧",
                             (unsigned)len, (unsigned)BEHAVIOR_INLINE_JSON_MAX);
        return false;
    }

    cJSON *root = cJSON_Parse(json_text);
    if (!root) {
        if (result) snprintf(result->error, sizeof(result->error),
                             "JSON 语法错误：无法解析，请检查括号、引号与逗号");
        return false;
    }
    if (!cJSON_IsObject(root)) {
        if (result) snprintf(result->error, sizeof(result->error),
                             "JSON 根节点必须是对象 { ... }");
        cJSON_Delete(root);
        return false;
    }

    /* 定位 timeline：裸对象 {"timeline":[…]} 或完整场景 {"presets":[{"timeline":[…]}]} */
    const cJSON *tl = cJSON_GetObjectItemCaseSensitive(root, "timeline");
    if (!cJSON_IsArray(tl)) {
        const cJSON *presets = cJSON_GetObjectItemCaseSensitive(root, "presets");
        if (cJSON_IsArray(presets) && cJSON_GetArraySize(presets) > 0) {
            tl = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(presets, 0), "timeline");
        }
    }
    if (!cJSON_IsArray(tl) || cJSON_GetArraySize(tl) <= 0) {
        if (result) snprintf(result->error, sizeof(result->error),
                             "缺少 timeline 数组：顶层需为 {\"timeline\":[…]} 或 {\"presets\":[{\"timeline\":[…]}]}");
        cJSON_Delete(root);
        return false;
    }
    int tl_count = cJSON_GetArraySize(tl);
    if (tl_count > BEHAVIOR_TIMELINE_MAX) {
        if (result) snprintf(result->error, sizeof(result->error),
                             "timeline 条目 %d 条，超过上限 %d 条", tl_count, BEHAVIOR_TIMELINE_MAX);
        cJSON_Delete(root);
        return false;
    }

    behavior_playback_t *pb = (behavior_playback_t *)bhv_calloc(1, sizeof(*pb));
    if (!pb) {
        if (result) snprintf(result->error, sizeof(result->error), "内存不足，无法构建播放体");
        cJSON_Delete(root);
        return false;
    }

    snprintf(pb->name, sizeof(pb->name), "%s",
             (scene_name && scene_name[0]) ? scene_name : BEHAVIOR_INLINE_SCENE_NAME);
    pb->kind     = BEHAVIOR_KIND_SCENE;
    pb->priority = PRIO_NORMAL;

    const cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "priority");
    action_priority_t pp;
    if (p && cJSON_IsString(p) && prio_from_str(p->valuestring, &pp)) pb->priority = pp;
    if (prio_override != BEHAVIOR_PRIO_DEFAULT) pb->priority = prio_override;

    const cJSON *config = cJSON_GetObjectItemCaseSensitive(root, "config");
    const cJSON *restore = config ? cJSON_GetObjectItemCaseSensitive(config, "restore_status") : NULL;
    if (restore && cJSON_IsString(restore) && restore->valuestring) {
        snprintf(pb->restore_status, sizeof(pb->restore_status), "%s", restore->valuestring);
        pb->has_restore = true;
    }
    const cJSON *no_restore = config ? cJSON_GetObjectItemCaseSensitive(config, "no_restore") : NULL;
    if (no_restore && cJSON_IsTrue(no_restore)) pb->no_restore = true;

    /* 逐条解析：asset_resolve 会读写 L1 缓存，需持锁 */
    int kf_total = 0;
    cache_lock();
    for (int i = 0; i < tl_count; i++) {
        load_timeline_item_inline(pb, cJSON_GetArrayItem(tl, i), &kf_total);
    }
    cache_unlock();
    cJSON_Delete(root);

    if (pb->item_count == 0) {
        if (result) snprintf(result->error, sizeof(result->error),
                             "没有任何有效条目：请检查 type 是否为 eye/iris/led/servo/motor/audio，"
                             "以及各通道关键帧字段名是否正确");
        playback_free(pb);
        return false;
    }

    /* 预估总时长：含 repeat=0 无限项时记为 0 */
    uint32_t total = 0;
    for (uint8_t i = 0; i < pb->item_count; i++) {
        const behavior_timeline_t *it = &pb->items[i];
        if (it->repeat == 0) { total = 0; break; }
        uint32_t one = it->channel->duration_ms(it->play_data);
        if (one == 0) one = 1;
        uint32_t end = it->time_ms + (uint32_t)it->repeat * one;
        if (end > total) total = end;
    }

    ESP_LOGI(TAG, "inline scene '%s': %u/%d items, %d keyframes, ~%ums, prio=%d%s",
             pb->name, (unsigned)pb->item_count, tl_count, kf_total, (unsigned)total,
             (int)pb->priority, pb->has_infinite ? " [infinite]" : "");

    /* pb 入队后即可能被行为任务接管并释放，统计量须在此前取好 */
    const int item_ok = (int)pb->item_count;

    if (!enqueue_pb(pb)) {
        if (result) snprintf(result->error, sizeof(result->error), "播放队列已满，请稍后重试");
        return false;
    }

    if (result) {
        result->ok             = true;
        result->item_total     = tl_count;
        result->item_count     = item_ok;
        result->keyframe_total = kf_total;
        result->duration_ms    = total;
    }
    return true;
}

bool app_behavior_play_status(const char *status_name)
{
    behavior_playback_t *pb = playback_load(status_name, BEHAVIOR_KIND_STATUS, NULL, BEHAVIOR_PRIO_DEFAULT);
    if (!pb) return false;
    return enqueue_pb(pb);
}

bool app_behavior_stop(action_priority_t drop_below)
{
    if (!s_bman.queue) return false;
    bmsg_t msg = { .type = BMSG_STOP, .drop_below = drop_below };
    return xQueueSend(s_bman.queue, &msg, 0) == pdTRUE;
}

bool app_behavior_is_idle(void)
{
    return s_idle;
}

const char *app_behavior_current_name(void)
{
    return s_bman.current ? s_bman.current->name : NULL;
}

action_priority_t app_behavior_current_priority(void)
{
    return s_bman.current ? s_bman.current->priority : PRIO_IDLE;
}

void app_behavior_set_scene_ended_hook(behavior_scene_ended_hook_t fn)
{
    s_scene_ended_hook = fn;
}

void app_behavior_set_scene_started_hook(behavior_scene_started_hook_t fn)
{
    s_scene_started_hook = fn;
}
