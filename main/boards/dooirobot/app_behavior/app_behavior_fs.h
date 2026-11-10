#ifndef APP_BEHAVIOR_FS_H_
#define APP_BEHAVIOR_FS_H_

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include <cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SPIFFS("behavior") 分区挂载点与资源目录约定 */
#define BEHAVIOR_FS_BASE      "/resources"
#define BEHAVIOR_FS_ASSET     "/resources/asset"
#define BEHAVIOR_FS_SCENES    "/resources/scenes"
#define BEHAVIOR_FS_STATUS    "/resources/status"

/**
 * @brief 初始化资源文件系统（挂载 behavior 分区到 /resources）
 *        幂等：已挂载时直接返回 ESP_OK。
 */
esp_err_t app_behavior_fs_init(void);

/** @brief 文件系统是否已挂载可用 */
bool app_behavior_fs_mounted(void);

/**
 * @brief 启动期预加载所有资源 JSON 文本到 PSRAM 缓存。
 *        幂等：重复调用先清空再填。应在 fs_init 之后调用。
 *        失败仅告警，不致命（miss 时 load_json 会回退读盘）。
 */
esp_err_t app_behavior_fs_preload(void);

/**
 * @brief 取某 rel_path 的缓存文本（只读视图，调用方不得修改/释放）。
 * @param rel_path 相对 /resources，如 "scenes/angry.json"
 * @param out_text 输出缓存内部指针
 * @param out_len  输出文本长度（可传 NULL）
 * @return true 命中；false 未命中
 * @note 拿到的指针在另一线程 save/remove 同 key 时可能失效，
 *       长时间持有或跨网络发送时请拷贝一份（见 app_behavior_fs_read_text）。
 */
bool app_behavior_fs_get_cached(const char *rel_path, const char **out_text, size_t *out_len);

/**
 * @brief 读取文件原文到新分配的 PSRAM 缓冲（调用方 heap_caps_free）。
 *        未命中缓存时回退读盘并顺带填充缓存。
 */
char *app_behavior_fs_read_text(const char *rel_path, size_t *out_len);

/**
 * @brief 读取 JSON 文件并解析为 cJSON 树。
 * @param rel_path 相对 /resources 的路径，如 "scenes/angry.json"
 * @return 成功返回 cJSON 指针（调用者负责 cJSON_Delete），失败返回 NULL
 */
cJSON *app_behavior_fs_load_json(const char *rel_path);

/**
 * @brief 把 cJSON 树序列化后写回文件（原子写：先写 .tmp 再 rename）。
 */
esp_err_t app_behavior_fs_save_json(const char *rel_path, const cJSON *json);

/**
 * @brief 把原始文本直接写入文件（原子写：.tmp + rename），并同步更新 PSRAM 缓存。
 *        供 web POST 等场景使用，无需经过 cJSON 树。
 */
esp_err_t app_behavior_fs_save_text(const char *rel_path, const char *text, size_t len);

/** @brief 检查文件是否存在 */
bool app_behavior_fs_exists(const char *rel_path);

/**
 * @brief 遍历已预取到 RAM 缓存中的文件，回调每个匹配 rel_prefix 的缓存键。
 *
 * 仅遍历内存缓存，不触达 SPIFFS，速度远快于 opendir/readdir。
 * 缓存由 app_behavior_fs_preload 在挂载/保存时填充，因此结果与磁盘一致且动态。
 *
 * @param rel_prefix  路径前缀，如 "scenes/action/"（传 "" 可遍历全部）
 * @param cb          对每个匹配的 rel_path 调用；返回 false 可提前结束遍历
 * @param arg         透传给 cb 的用户参数
 */
void app_behavior_fs_foreach_cached(const char *rel_prefix,
                                    bool (*cb)(const char *rel_path, void *arg),
                                    void *arg);

/** @brief 删除文件（不存在也返回 ESP_OK） */
esp_err_t app_behavior_fs_remove(const char *rel_path);

/* ==================== 路径助手 ==================== */
size_t app_behavior_fs_asset_path(char *buf, size_t bufsz, const char *channel, const char *asset_id);
size_t app_behavior_fs_scene_path(char *buf, size_t bufsz, const char *name);
size_t app_behavior_fs_status_path(char *buf, size_t bufsz, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* APP_BEHAVIOR_FS_H_ */