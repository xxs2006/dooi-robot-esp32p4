#ifndef APP_BEHAVIOR_TYPES_H_
#define APP_BEHAVIOR_TYPES_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 松散声明：具体定义见 cJSON.h / behavior_channel.h */
typedef struct cJSON cJSON;
struct behavior_channel;
typedef struct behavior_channel behavior_channel_t;

/* ==================== 常量 ==================== */
#define BEHAVIOR_NAME_MAX         32
#define BEHAVIOR_ASSET_MAX        48
#define BEHAVIOR_TIMELINE_MAX     16
#define BEHAVIOR_PRESET_MAX      8
#define BEHAVIOR_QUEUE_DEPTH      8
#define BEHAVIOR_STATUS_STACK_DEPTH 8

/* 特殊优先级：表示“采用 JSON 中配置的优先级”（用于 prio_override 参数） */
#define BEHAVIOR_PRIO_DEFAULT     ((action_priority_t)0xFF)

/* ==================== 优先级 ==================== */
typedef enum {
    PRIO_IDLE = 0,
    PRIO_LOW,
    PRIO_NORMAL,
    PRIO_HIGH,
    PRIO_CRITICAL
} action_priority_t;

/* ==================== 行为种类 ==================== */
typedef enum {
    BEHAVIOR_KIND_SCENE  = 0,      /* 场景：由触发源请求，可打断低优行为 */
    BEHAVIOR_KIND_STATUS,          /* 状态：最底层基线，用于结束后恢复硬件 */
} behavior_kind_t;

typedef enum {
    BEHAVIOR_EVT_STARTED = 0,
    BEHAVIOR_EVT_FINISHED,
    BEHAVIOR_EVT_INTERRUPTED,
} behavior_event_type_t;

/* 行为事件（观察者订阅） */
typedef struct {
    behavior_event_type_t type;
    const char           *name;
    const char           *preset;
    behavior_kind_t       kind;
    action_priority_t     priority;
} behavior_event_t;

/* 行为结束结果（透传给状态恢复钩子） */
typedef struct {
    char             name[BEHAVIOR_NAME_MAX];
    char             preset[BEHAVIOR_NAME_MAX];
    behavior_kind_t  kind;
    bool             was_interrupted;
    bool             has_infinite;    /* 含 repeat=0 无限项，只能被外部打断 */
    uint32_t         elapsed_ms;
} behavior_result_t;

/* ==================== 时间轴条目（含运行时状态） ==================== */
typedef struct {
    /* --- 配置（由加载器填写） --- */
    uint32_t                 time_ms;      /* 相对行为开始的延时(ms) */
    uint8_t                  repeat;       /* 0 = 无限循环 */
    char                     asset[BEHAVIOR_ASSET_MAX];
    const behavior_channel_t *channel;

    /* --- 运行时（管理器维护） --- */
    cJSON   *asset_json;      /* 解析后的 asset（仅非缓存自有条目持有，可 NULL） */
    void    *play_data;       /* channel->parse() 结果 */
    bool     owned_play;      /* true: play_data 本条目自有，结束须 destroy；false: 借用驻留缓存，不释放 */
    bool     started;         /* 已到 time_ms 并进入播放 */
    bool     done;            /* 指定次数播完 */
    uint64_t start_wall_ms;   /* pass0 起始墙钟时间 */
    uint32_t last_pass;       /* 上一 tick 所处的 pass（用于循环游标切换） */
    int      last_idx;        /* 已触发到的 keyframe 游标 */
} behavior_timeline_t;

/* ==================== 行为播放体 ==================== */
typedef struct {
    char                 name[BEHAVIOR_NAME_MAX];
    behavior_kind_t      kind;
    char                 preset[BEHAVIOR_NAME_MAX];
    action_priority_t    priority;
    char                 restore_status[BEHAVIOR_NAME_MAX]; /* 场景结束后显式恢复的状态 */
    bool                 has_restore;
    char                 preload_assets[BEHAVIOR_NAME_MAX]; /* 场景前置基线状态(资源名,无.json) */
    bool                 has_preload;    /* 是否声明了 preload_assets */
    bool                 no_restore;     /* 场景结束后不触发状态恢复钩子（瞬态/纯音效场景用） */
    bool                 has_infinite;   /* 任意条目 repeat==0 */
    behavior_timeline_t  items[BEHAVIOR_TIMELINE_MAX];
    uint8_t              item_count;

    uint64_t             start_wall_ms;
} behavior_playback_t;

#ifdef __cplusplus
}
#endif

#endif /* APP_BEHAVIOR_TYPES_H_ */