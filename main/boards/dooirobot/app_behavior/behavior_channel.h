#ifndef BEHAVIOR_CHANNEL_H_
#define BEHAVIOR_CHANNEL_H_

#include <stdint.h>
#include <stdbool.h>
#include <cJSON.h>
#include "app_behavior_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 硬件通道抽象：一个 channel = 一类子硬件（eye / led / servo / motor / audio）。
 *
 * 生命周期（均由 app_behavior 管理器在行为任务上下文调用）：
 *   parse()             -> 把 asset JSON 解析为通道私有播放数据（堆分配），失败返回 NULL
 *   keyframe_count()    -> 该 asset 的单遍 keyframe 数量
 *   kf_time_ms(idx)     -> 第 idx 个 keyframe 在单遍内的本地时间戳(ms)
 *   apply(idx)          -> 触发第 idx 个 keyframe 到硬件（管理器保证游标单调递增，不重入）
 *   duration_ms()       -> 单遍总时长(ms)；始终 >=1，供管理器做 pass/循环调度
 *   destroy(play,intr)  -> 结束时释放播放数据；intr=true 表示被打断，须立即把硬件停在安全态
 */
struct behavior_channel {
    const char *name;

    void *(*parse)(const cJSON *asset);
    int   (*keyframe_count)(const void *play);
    uint32_t (*kf_time_ms)(const void *play, int idx);
    void  (*apply)(void *play, int idx);
    uint32_t (*duration_ms)(const void *play);
    void  (*destroy)(void *play, bool interrupted);

    /* stop(): 停止本通道硬件（如停音/停电机/关灯），但【不释放 play】。
     * 由于 play_data 可能来自 PSRAM 驻留缓存、被多个播放体共享，
     * 结束/打断时应调用 stop() 复位硬件，而非 destroy()（destroy 只应
     * 用于释放非缓存的自有 play_data）。 */
    void  (*stop)(void *play, bool interrupted);
};

/** @brief 按名称查找通道（eye/led/servo/motor/audio），找不到返回 NULL */
const behavior_channel_t *behavior_channel_find(const char *name);

/* ==================== 通用解析助手（供各通道实现） ==================== */

bool behavior_kf_get_u32(const cJSON *kf, const char *key, uint32_t *out);
bool behavior_kf_get_i16(const cJSON *kf, const char *key, int16_t *out);
bool behavior_kf_get_i32(const cJSON *kf, const char *key, int32_t *out);
bool behavior_kf_get_i64(const cJSON *kf, const char *key, int64_t *out);
bool behavior_kf_get_f64(const cJSON *kf, const char *key, double *out);
/** 兼容 duration / duration_ms 两种字段 */
bool behavior_kf_get_duration_ms(const cJSON *kf, uint32_t *out);

/**
 * @brief 解析颜色：支持数字 0xRRGGBB、字符串 "#RRGGBB" / "0xRRGGBB" / 命名色(red,green,blue...)
 */
bool behavior_parse_color(const cJSON *kf, const char *key, uint32_t *rgb_out);

/** @brief 读取时间轴项的通道名（兼容 type / channel 字段） */
bool behavior_parse_channel_name(const cJSON *timeline_item, const char **out);

#ifdef __cplusplus
}
#endif

#endif /* BEHAVIOR_CHANNEL_H_ */