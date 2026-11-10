#include "app_behavior_fs.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "behavior_fs";

#define BEHAVIOR_PARTITION   "behavior"
#define BEHAVIOR_MAX_FILES   16
#define BEHAVIOR_FILE_MAX    (32 * 1024)   /* 单个 JSON 上限，防失控 */

/* PSRAM 文本缓存：启动期预读全部资源 JSON，播放/网页读取零 flash I/O */
#define BEHAVIOR_CACHE_CAP_BYTES  (512 * 1024)   /* 总量上限（实际 ~63KB） */
#define BEHAVIOR_CACHE_INIT_CAP   64             /* 初始槽位 */

typedef struct {
    char    *rel_path;   /* 缓存键，如 "scenes/angry.json"，PSRAM */
    char    *text;       /* 原始 JSON 文本，NUL 结尾，PSRAM */
    size_t   len;        /* 文本长度（不含 NUL） */
} behavior_cache_entry_t;

static behavior_cache_entry_t *s_cache = NULL;   /* 动态数组，PSRAM */
static size_t s_cache_count       = 0;
static size_t s_cache_cap         = 0;
static size_t s_cache_total_bytes = 0;

static SemaphoreHandle_t s_fs_lock = NULL;
static bool s_mounted = false;

static void fs_lock(void)   { xSemaphoreTakeRecursive(s_fs_lock, portMAX_DELAY); }
static void fs_unlock(void) { xSemaphoreGiveRecursive(s_fs_lock); }

/* ==================== 缓存内部操作（调用方须持 s_fs_lock） ==================== */

static const behavior_cache_entry_t *cache_find(const char *rel_path)
{
    for (size_t i = 0; i < s_cache_count; i++) {
        if (strcmp(s_cache[i].rel_path, rel_path) == 0) return &s_cache[i];
    }
    return NULL;
}

/* 插入或更新。text 由调用方提供，cache_put 内部拷贝一份到 PSRAM。 */
static esp_err_t cache_put(const char *rel_path, const char *text, size_t len)
{
    /* 更新已存在项 */
    for (size_t i = 0; i < s_cache_count; i++) {
        if (strcmp(s_cache[i].rel_path, rel_path) == 0) {
            char *new_text = (char *)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!new_text) { ESP_LOGE(TAG, "cache_put OOM (update)"); return ESP_ERR_NO_MEM; }
            memcpy(new_text, text, len);
            new_text[len] = '\0';
            heap_caps_free(s_cache[i].text);
            s_cache[i].text = new_text;
            s_cache_total_bytes += (len - s_cache[i].len);
            s_cache[i].len = len;
            return ESP_OK;
        }
    }

    /* 扩容 */
    if (s_cache_count >= s_cache_cap) {
        size_t new_cap = s_cache_cap * 2;
        behavior_cache_entry_t *nb = (behavior_cache_entry_t *)heap_caps_realloc(
            s_cache, new_cap * sizeof(*nb), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb) { ESP_LOGE(TAG, "cache grow OOM"); return ESP_ERR_NO_MEM; }
        memset(nb + s_cache_cap, 0, (new_cap - s_cache_cap) * sizeof(*nb));
        s_cache = nb;
        s_cache_cap = new_cap;
    }

    /* 插入新项 */
    char *rp = (char *)heap_caps_malloc(strlen(rel_path) + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *tx = (char *)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rp || !tx) {
        heap_caps_free(rp);
        heap_caps_free(tx);
        ESP_LOGE(TAG, "cache insert OOM");
        return ESP_ERR_NO_MEM;
    }
    strcpy(rp, rel_path);
    memcpy(tx, text, len);
    tx[len] = '\0';
    s_cache[s_cache_count].rel_path = rp;
    s_cache[s_cache_count].text     = tx;
    s_cache[s_cache_count].len      = len;
    s_cache_count++;
    s_cache_total_bytes += len;

    if (s_cache_total_bytes > BEHAVIOR_CACHE_CAP_BYTES) {
        ESP_LOGW(TAG, "cache total %u > cap %u", (unsigned)s_cache_total_bytes,
                 (unsigned)BEHAVIOR_CACHE_CAP_BYTES);
    }
    return ESP_OK;
}

static void cache_drop(const char *rel_path)
{
    for (size_t i = 0; i < s_cache_count; i++) {
        if (strcmp(s_cache[i].rel_path, rel_path) == 0) {
            s_cache_total_bytes -= s_cache[i].len;
            heap_caps_free(s_cache[i].text);
            heap_caps_free(s_cache[i].rel_path);
            s_cache[i] = s_cache[s_cache_count - 1];   /* swap-pop */
            s_cache_count--;
            return;
        }
    }
}

/* 读整个文件到堆（PSRAM 优先），返回需调 heap_caps_free */
static char *fs_read_all(const char *rel_path, size_t *out_len)
{
    char path[320];
    snprintf(path, sizeof(path), "%s/%s", BEHAVIOR_FS_BASE, rel_path);

    struct stat st;
    if (stat(path, &st) != 0) return NULL;
    if (st.st_size <= 0 || st.st_size > BEHAVIOR_FILE_MAX) {
        ESP_LOGW(TAG, "bad size for %s: %ld", path, (long)st.st_size);
        return NULL;
    }

    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    char *buf = heap_caps_malloc((size_t)st.st_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "OOM %d bytes for %s", (int)st.st_size + 1, path);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

/* 遍历 /resources/<dir_rel> 下所有文件，读入缓存 */
static void cache_walk_dir(const char *dir_rel)
{
    char dir[320];
    snprintf(dir, sizeof(dir), "%s/%s", BEHAVIOR_FS_BASE, dir_rel);

    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGW(TAG, "preload: opendir %s failed", dir);
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (name[0] == '.') continue;              /* skip . / .. */
        if (strstr(name, ".tmp")) continue;        /* 跳过原子写残留 */

        /* dir_rel 最长 "asset/audio"=11，name 最长 NAME_MAX=255，加 '/' 与 NUL */
        char rel[300];
        snprintf(rel, sizeof(rel), "%s/%s", dir_rel, name);

        /* 递归进入子目录（如 scenes/emotion、scenes/action） */
        char abs_path[320];
        snprintf(abs_path, sizeof(abs_path), "%s/%s", BEHAVIOR_FS_BASE, rel);
        struct stat st;
        if (stat(abs_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            cache_walk_dir(rel);
            continue;
        }

        size_t len = 0;
        char *buf = fs_read_all(rel, &len);
        if (buf) {
            cache_put(rel, buf, len);   /* 内部拷贝 */
            heap_caps_free(buf);
        }
    }
    closedir(d);
}

esp_err_t app_behavior_fs_init(void)
{
    if (s_mounted) return ESP_OK;
    if (!s_fs_lock) s_fs_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_fs_lock) return ESP_ERR_NO_MEM;

    if (esp_spiffs_mounted(BEHAVIOR_PARTITION)) {
        s_mounted = true;
        ESP_LOGI(TAG, "already mounted: %s @ %s", BEHAVIOR_PARTITION, BEHAVIOR_FS_BASE);
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = BEHAVIOR_FS_BASE,
        .partition_label = BEHAVIOR_PARTITION,
        .max_files = BEHAVIOR_MAX_FILES,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount %s @ %s failed: %s",
                 BEHAVIOR_PARTITION, BEHAVIOR_FS_BASE, esp_err_to_name(ret));
        return ret;
    }
    s_mounted = true;
    ESP_LOGI(TAG, "mounted %s @ %s", BEHAVIOR_PARTITION, BEHAVIOR_FS_BASE);
    return ESP_OK;
}

bool app_behavior_fs_mounted(void)
{
    return s_mounted;
}

/* 启动期预加载所有资源 JSON 文本到 PSRAM。幂等：重复调用先清空再填。 */
esp_err_t app_behavior_fs_preload(void)
{
    if (!app_behavior_fs_mounted()) return ESP_ERR_INVALID_STATE;

    fs_lock();

    /* 清空旧缓存（若有），保留表 */
    if (s_cache) {
        for (size_t i = 0; i < s_cache_count; i++) {
            heap_caps_free(s_cache[i].text);
            heap_caps_free(s_cache[i].rel_path);
        }
        s_cache_count = 0;
        s_cache_total_bytes = 0;
    } else {
        s_cache_cap = BEHAVIOR_CACHE_INIT_CAP;
        s_cache = (behavior_cache_entry_t *)heap_caps_calloc(
            s_cache_cap, sizeof(*s_cache), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_cache) {
            fs_unlock();
            ESP_LOGE(TAG, "preload: cache table OOM");
            return ESP_ERR_NO_MEM;
        }
    }

    static const char *dirs[] = {
        "asset/audio", "asset/eye", "asset/led", "asset/motor", "asset/servo",
        "scenes", "status",
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        cache_walk_dir(dirs[i]);
    }

    ESP_LOGI(TAG, "preload done: %u files, %u bytes",
             (unsigned)s_cache_count, (unsigned)s_cache_total_bytes);
    fs_unlock();
    return ESP_OK;
}

/* 取缓存文本（只读视图，调用方不得修改/释放）。
 * 命中返回 true。供 load_json / web GET 复用。 */
bool app_behavior_fs_get_cached(const char *rel_path, const char **out_text, size_t *out_len)
{
    if (!rel_path || !out_text) return false;
    fs_lock();
    const behavior_cache_entry_t *e = cache_find(rel_path);
    bool hit = false;
    if (e) {
        *out_text = e->text;
        if (out_len) *out_len = e->len;
        hit = true;
    }
    fs_unlock();
    return hit;
}

/* 读文件原文到新分配的 PSRAM 缓冲（调用方 heap_caps_free）。
 * 未命中缓存时回退读盘并顺带填充缓存。供 web GET fallback 使用。 */
char *app_behavior_fs_read_text(const char *rel_path, size_t *out_len)
{
    if (!rel_path || !app_behavior_fs_mounted()) return NULL;

    fs_lock();
    const behavior_cache_entry_t *e = cache_find(rel_path);
    if (e) {
        char *copy = (char *)heap_caps_malloc(e->len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (copy) {
            memcpy(copy, e->text, e->len);
            copy[e->len] = '\0';
            if (out_len) *out_len = e->len;
        }
        fs_unlock();
        return copy;
    }
    /* miss: 读盘并填充缓存 */
    size_t len = 0;
    char *buf = fs_read_all(rel_path, &len);
    if (buf) {
        cache_put(rel_path, buf, len);
        if (out_len) *out_len = len;
    }
    fs_unlock();
    return buf;
}

cJSON *app_behavior_fs_load_json(const char *rel_path)
{
    if (!rel_path || !app_behavior_fs_mounted()) return NULL;

    fs_lock();

    /* 优先走 PSRAM 缓存，零 flash I/O */
    const behavior_cache_entry_t *e = cache_find(rel_path);
    if (e) {
        cJSON *root = cJSON_ParseWithLength(e->text, e->len);
        fs_unlock();
        if (!root) ESP_LOGE(TAG, "parse failed (cached): %s", rel_path);
        return root;
    }

    /* miss: 读盘并填充缓存 */
    size_t len = 0;
    char *buf = fs_read_all(rel_path, &len);
    if (!buf) { fs_unlock(); return NULL; }

    cJSON *root = cJSON_ParseWithLength(buf, len);
    if (root) {
        cache_put(rel_path, buf, len);   /* 内部拷贝 */
    }
    heap_caps_free(buf);
    fs_unlock();

    if (!root) ESP_LOGE(TAG, "parse failed: %s", rel_path);
    return root;
}

void app_behavior_fs_foreach_cached(const char *rel_prefix,
                                    bool (*cb)(const char *rel_path, void *arg),
                                    void *arg)
{
    if (!cb || !app_behavior_fs_mounted()) return;

    size_t pre_len = rel_prefix ? strlen(rel_prefix) : 0;

    fs_lock();
    for (size_t i = 0; i < s_cache_count; i++) {
        const char *key = s_cache[i].rel_path;
        if (!key) continue;
        if (pre_len && strncmp(key, rel_prefix, pre_len) != 0) continue;
        /* 缓存数组在本函数执行期间不会被释放/改写，可直接把 const 指针交给回调 */
        fs_unlock();
        if (!cb(key, arg)) return;          /* 调用方要求提前结束 */
        fs_lock();
    }
    fs_unlock();
}

esp_err_t app_behavior_fs_save_json(const char *rel_path, const cJSON *json)
{
    if (!rel_path || !json || !app_behavior_fs_mounted()) return ESP_ERR_INVALID_ARG;

    char *text = cJSON_PrintUnformatted(json);
    if (!text) return ESP_ERR_NO_MEM;

    esp_err_t ret = app_behavior_fs_save_text(rel_path, text, strlen(text));
    cJSON_free(text);
    return ret;
}

/* 把原始文本直接写入文件，并同步更新 PSRAM 缓存。
 * 供 web POST 等场景使用，无需经过 cJSON 树。
 *
 * ★ SPIFFS 不支持跨文件 rename 原子写（rename 到已存在文件行为不可靠），
 *   故直接以 "w" 截断覆盖写。fopen("w") 在 SPIFFS 上等价于先删后建，
 *   写入过程中断会留下空文件，但资源 JSON 体量小，可接受。 */
esp_err_t app_behavior_fs_save_text(const char *rel_path, const char *text, size_t len)
{
    if (!rel_path || !text || !app_behavior_fs_mounted()) return ESP_ERR_INVALID_ARG;
    if (len > BEHAVIOR_FILE_MAX) {
        ESP_LOGW(TAG, "save_text: %s too large (%u)", rel_path, (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }

    char path[320];
    snprintf(path, sizeof(path), "%s/%s", BEHAVIOR_FS_BASE, rel_path);

    esp_err_t ret = ESP_OK;
    fs_lock();
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "save_text: fopen(%s) failed errno=%d", path, errno);
        ret = ESP_FAIL;
        goto out;
    }
    size_t nw = fwrite(text, 1, len, f);
    fclose(f);
    if (nw != len) {
        ESP_LOGE(TAG, "save_text: short write %u/%u for %s (errno=%d)",
                 (unsigned)nw, (unsigned)len, path, errno);
        remove(path);
        ret = ESP_FAIL;
        goto out;
    }
    /* 写入成功，同步更新缓存 */
    cache_put(rel_path, text, len);
    ESP_LOGI(TAG, "save_text: %s (%u bytes) ok", path, (unsigned)len);
out:
    fs_unlock();
    return ret;
}

bool app_behavior_fs_exists(const char *rel_path)
{
    if (!rel_path) return false;
    /* 优先查 PSRAM 缓存：所有资源 JSON 已在启动期预加载，命中即零 flash I/O。
     * 这是场景路径探测(scene_path)的主要调用路径，SPIFFS stat 较慢，
     * 若直接 stat 会导致每次按键在路径探测上卡数百毫秒。 */
    if (s_mounted) {
        fs_lock();
        bool hit = (cache_find(rel_path) != NULL);
        fs_unlock();
        if (hit) return true;
    }
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", BEHAVIOR_FS_BASE, rel_path);
    struct stat st;
    return stat(path, &st) == 0;
}

esp_err_t app_behavior_fs_remove(const char *rel_path)
{
    if (!rel_path || !app_behavior_fs_mounted()) return ESP_ERR_INVALID_ARG;
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", BEHAVIOR_FS_BASE, rel_path);
    fs_lock();
    int r = remove(path);
    if (r == 0) cache_drop(rel_path);
    fs_unlock();
    return (r == 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

size_t app_behavior_fs_asset_path(char *buf, size_t bufsz, const char *channel, const char *asset_id)
{
    return (size_t)snprintf(buf, bufsz, "asset/%s/%s", channel, asset_id);
}

// 场景名 -> 相对路径。场景按类别存放在 scenes/emotion/ 与 scenes/action/ 下，
// 这里自动匹配所在目录；未命中时回退到扁平的 scenes/<name>.json（向后兼容）。
// 注意：emotion 与 action 类别下的文件已统一加 "emotion_" / "action_" 前缀
// （如 emotion_angry.json、action_greeting.json），查找时优先尝试该前缀名，
// 再回退到无前缀名，保持向后兼容。
/* 场景类别目录。emotion/action 对外（含 AI 可调用的动作），sys 为内部/系统
 * 场景（factory_reset、game_*、tomato_tick 等），仅由内部代码调用，不暴露给 AI。 */
static const char *k_scene_categories[] = {"emotion", "action", "sys"};

/* 纯缓存命中式存在判断（绝不触碰 SPIFFS）。
 * 所有资源 JSON 在启动期已由 preload 预加载进 PSRAM 缓存，故对场景文件做
 * 存在性判断只需查缓存即可，精确且零 flash I/O，避免对不存在的候选路径
 * 反复执行慢速 SPIFFS stat（每次数百 ms）。 */
static bool cached_exists(const char *rel_path)
{
    const char *t = NULL;
    size_t l = 0;
    return app_behavior_fs_get_cached(rel_path, &t, &l);
}

size_t app_behavior_fs_scene_path(char *buf, size_t bufsz, const char *name)
{
    if (!app_behavior_fs_mounted()) return 0;
    char candidate[128];

    /* 主路径 0：sys 场景文件为裸名（无 "sys_" 前缀），如 tomato_tick、
     * factory_reset、game_* 等，位于 scenes/sys/<name>.json。走纯缓存判断，
     * 命中即返回，避免落入下方逐候选 SPIFFS stat 的慢路径（~1s+）。
     * 番茄钟每秒触发一次，此处若慢会直接把调用间隔拉长到数秒。 */
    snprintf(candidate, sizeof(candidate), "scenes/sys/%s.json", name);
    if (cached_exists(candidate)) {
        return (size_t)snprintf(buf, bufsz, "%s", candidate);
    }

    /* 主路径：scene 名已带 "action_" / "emotion_" 前缀，直接确定性拼接，
     * 不做 exists() 探测。SPIFFS 的 stat 极慢，逐个探测不存在的候选路径
     * 会让每次按键在路径解析上卡约 1s（playback_load 的首个阻塞点）。 */
    for (int i = 0; i < (int)(sizeof(k_scene_categories) / sizeof(k_scene_categories[0])); i++) {
        const char *cat = k_scene_categories[i];
        if (strncmp(name, cat, strlen(cat)) == 0 && name[strlen(cat)] == '_') {
            snprintf(candidate, sizeof(candidate), "scenes/%s/%s.json", cat, name);
            if (cached_exists(candidate)) {
                return (size_t)snprintf(buf, bufsz, "%s", candidate);
            }
        }
    }

    /* 兼容回退（旧的无前缀 scene 名）：按类别探测 <cat>_<name> 再 <name>。
     * 一律走纯缓存判断——所有场景 JSON 已预加载进 PSRAM 缓存，缓存 miss 即
     * 等价于文件不存在，绝不再回退到慢速 SPIFFS stat（每次数百 ms）。 */
    for (int i = 0; i < (int)(sizeof(k_scene_categories) / sizeof(k_scene_categories[0])); i++) {
        const char *cat = k_scene_categories[i];
        snprintf(candidate, sizeof(candidate), "scenes/%s/%s_%s.json", cat, cat, name);
        if (cached_exists(candidate)) {
            return (size_t)snprintf(buf, bufsz, "%s", candidate);
        }
        snprintf(candidate, sizeof(candidate), "scenes/%s/%s.json", cat, name);
        if (cached_exists(candidate)) {
            return (size_t)snprintf(buf, bufsz, "%s", candidate);
        }
    }
    // 兜底：未分类的扁平场景
    snprintf(candidate, sizeof(candidate), "scenes/%s.json", name);
    if (cached_exists(candidate)) {
        return (size_t)snprintf(buf, bufsz, "%s", candidate);
    }
    return 0;
}

size_t app_behavior_fs_status_path(char *buf, size_t bufsz, const char *name)
{
    return (size_t)snprintf(buf, bufsz, "status/%s.json", name);
}
