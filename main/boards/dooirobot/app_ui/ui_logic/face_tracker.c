#include "face_tracker.h"

#include <string.h>
#include "esp_log.h"
#include "lvgl.h"           // lv_tick_get()
#include "app_ui_logic.h"   // app_ui_logic_post, APP_EVT_FACE_ALIGNED
#include "app_motor.h"      // app_motor_turn_angle, app_motor_stop

static const char *TAG = "face_tracker";

//====================================================
// 单例实例
//====================================================
typedef struct {
    bool               enabled;
    bool               continuous;
    bool               initialized;
    face_track_state_t state;

    uint32_t           correct_start_time;
    uint32_t           last_turn_time;
    uint32_t           last_face_seen_time;

    vision_result_t    cached_face;
    bool               cached_face_valid;
    uint32_t           cached_face_time;

    vision_result_t    external_face;
    bool               external_face_valid;
    uint32_t           external_face_time;

    face_tracker_config_t cfg;
} face_tracker_t;

static face_tracker_t s_tracker = {0};

/* 状态机"重新武装"：置为 CORRECTING 并重置所有计时器 */
static void priv_arm(bool continuous);

//====================================================
// 初始化
//====================================================
void face_tracker_init(const face_tracker_config_t *cfg)
{
    const bool first_init = !s_tracker.initialized;

    if (first_init) {
        memset(&s_tracker, 0, sizeof(s_tracker));
    }

    /* cfg == NULL 表示"解绑回调"。页面卸载后其 LVGL 对象可能已被销毁，
       必须清空回调，否则 on_status 会对悬空 label 调用 lv_label_set_text 而崩溃。 */
    if (cfg) {
        s_tracker.cfg = *cfg;
    } else {
        memset(&s_tracker.cfg, 0, sizeof(s_tracker.cfg));
    }

    s_tracker.initialized = true;

    if (cfg) {
        /* 注入有效回调后自动启用并重新武装状态机。
           用 priv_arm() 而不是 face_tracker_enable()：后者在
           (enabled, continuous) 未变化时会提前返回，不会重置状态机，
           导致从 LOST / 过期 CORRECTING 恢复不及时。 */
        priv_arm(true);
        ESP_LOGI(TAG, "face_tracker %s (tracking enabled)",
                 first_init ? "initialized" : "config updated");
    } else {
        /* 无数据源，不改变启用状态（由调用方决定何时 enable） */
        ESP_LOGI(TAG, "face_tracker callbacks unbound");
    }
}

//====================================================
// 解绑回调（页面卸载时调用）
// 只清回调、不停用追踪：跨页面追踪依赖 face_tracker_push_result()，
// 清空 cfg 后 priv_get_offset() 会自动改走 external_face 分支。
//====================================================
void face_tracker_unbind(void)
{
    if (!s_tracker.initialized) return;

    memset(&s_tracker.cfg, 0, sizeof(s_tracker.cfg));
    s_tracker.cached_face_valid = false;   /* 旧的 map_display 已失效 */

    ESP_LOGI(TAG, "[FaceTrack] Callbacks unbound (cross-page tracking %s)",
             s_tracker.enabled ? "active" : "inactive");
}

//====================================================
// 反初始化：停用 + 解绑回调
//====================================================
void face_tracker_deinit(void)
{
    if (!s_tracker.initialized) return;

    /* enable() 在已停用时会提前返回，这里显式停一次电机 */
    if (s_tracker.enabled || s_tracker.continuous) {
        face_tracker_enable(false, false);
    }
    app_motor_stop();

    s_tracker.state               = FACE_TRACK_STATE_IDLE;
    s_tracker.cached_face_valid   = false;
    s_tracker.external_face_valid = false;
    memset(&s_tracker.cfg, 0, sizeof(s_tracker.cfg));
    s_tracker.initialized         = false;

    ESP_LOGI(TAG, "[FaceTrack] Deinitialized");
}

//====================================================
// 内部：获取人脸中心偏移
// 返回 true 表示本次有有效的缓存/新鲜人脸数据
//====================================================
static bool priv_get_offset(int *out_dx, int *out_dy)
{
    uint32_t now = lv_tick_get();

    vision_result_t fresh = {0};
    bool has_fresh = false;

    if (s_tracker.cfg.get_result) {
        has_fresh = s_tracker.cfg.get_result(&fresh, s_tracker.cfg.user_data);
    } else if (s_tracker.external_face_valid &&
               (now - s_tracker.external_face_time) <= FACE_TRACK_FACE_CACHE_VALID_MS) {
        has_fresh = true;
        memcpy(&fresh, &s_tracker.external_face, sizeof(vision_result_t));
    }

    if (has_fresh) {
        memcpy(&s_tracker.cached_face, &fresh, sizeof(vision_result_t));
        s_tracker.cached_face_valid = true;
        s_tracker.cached_face_time  = now;
    }

    if (!s_tracker.cached_face_valid ||
        (now - s_tracker.cached_face_time) > FACE_TRACK_FACE_CACHE_VALID_MS) {
        return false;
    }

    const vision_result_t *r = &s_tracker.cached_face;
    int dx = 0, dy = 0, dw = 0, dh = 0;

    if (s_tracker.cfg.map_display) {
        s_tracker.cfg.map_display(r->box[0], r->box[1],
                                  r->box[2] - r->box[0],
                                  r->box[3] - r->box[1],
                                  &dx, &dy, &dw, &dh,
                                  s_tracker.cfg.user_data);
    } else {
        dx = r->box[0];
        dy = r->box[1];
        dw = r->box[2] - r->box[0];
        dh = r->box[3] - r->box[1];
    }

    *out_dx = (dx + dw / 2) - FACE_TRACK_CENTER_X;
    *out_dy = (dy + dh / 2) - FACE_TRACK_CENTER_Y;
    return true;
}

//====================================================
// 内部：发送转向指令
// 返回实际下发的角度（0 表示在死区内，未转动）
//====================================================
static float priv_send_turn(int dx)
{
    int abs_dx = dx < 0 ? -dx : dx;
    if (abs_dx <= FACE_TRACK_DEAD_ZONE) return 0.0f;

    float angle = (float)(dx - (dx > 0 ? FACE_TRACK_DEAD_ZONE : -FACE_TRACK_DEAD_ZONE))
                  * FACE_TRACK_PIXEL_TO_ANGLE;

    if (angle >  FACE_TRACK_MAX_ANGLE) angle =  FACE_TRACK_MAX_ANGLE;
    if (angle < -FACE_TRACK_MAX_ANGLE) angle = -FACE_TRACK_MAX_ANGLE;

    float abs_angle = angle < 0.0f ? -angle : angle;
    if (abs_angle < FACE_TRACK_MIN_ANGLE) return 0.0f;

    app_motor_turn_angle(angle, FACE_TRACK_TURN_SPEED);
    s_tracker.last_turn_time = lv_tick_get();
    return angle;
}

//====================================================
// 启用 / 停用
//====================================================
static void priv_arm(bool continuous)
{
    uint32_t now = lv_tick_get();

    s_tracker.enabled             = true;
    s_tracker.continuous          = continuous;
    s_tracker.state               = FACE_TRACK_STATE_CORRECTING;
    s_tracker.correct_start_time  = now;
    s_tracker.last_face_seen_time = now;
    s_tracker.last_turn_time      = 0;
    s_tracker.cached_face_valid   = false;

    ESP_LOGI(TAG, "[FaceTrack] Enabled (%s)", continuous ? "Continuous" : "Once");

    int dx = 0, dy = 0;
    if (priv_get_offset(&dx, &dy)) {
        priv_send_turn(dx);
    } else {
        ESP_LOGI(TAG, "[FaceTrack] Waiting for first face frame...");
    }
}

void face_tracker_enable(bool enable, bool continuous)
{
    if (!s_tracker.initialized) {
        ESP_LOGW(TAG, "[FaceTrack] Not initialized, call face_tracker_init first");
        return;
    }

    if (enable) {
        if (s_tracker.enabled && s_tracker.continuous == continuous) return;
        priv_arm(continuous);
    } else {
        if (!s_tracker.enabled) return;
        s_tracker.enabled    = false;
        s_tracker.state      = FACE_TRACK_STATE_IDLE;
        s_tracker.cached_face_valid = false;
        s_tracker.external_face_valid = false;
        app_motor_stop();
        ESP_LOGI(TAG, "[FaceTrack] Disabled");
    }
}

//====================================================
// 每帧驱动状态机
//====================================================
void face_tracker_update(void)
{
    if (!s_tracker.initialized || !s_tracker.enabled) return;

    uint32_t now     = lv_tick_get();
    int      dx = 0, dy = 0;
    bool     has_face = priv_get_offset(&dx, &dy);
    int      abs_dx   = dx < 0 ? -dx : dx;

    switch (s_tracker.state) {

        case FACE_TRACK_STATE_CORRECTING:
            if (!has_face) {
                if ((now - s_tracker.correct_start_time) > FACE_TRACK_CORRECT_TIMEOUT_MS) {
                    ESP_LOGW(TAG, "[FaceTrack] Correction timeout, no face found");
                    app_motor_stop();
                    s_tracker.state = FACE_TRACK_STATE_LOST;
                    if (s_tracker.cfg.on_status)
                        s_tracker.cfg.on_status("等待检测...", s_tracker.cfg.user_data);
                }
                break;
            }

            s_tracker.last_face_seen_time = now;

            if (abs_dx <= FACE_TRACK_DEAD_ZONE) {
                s_tracker.state          = FACE_TRACK_STATE_TRACKING;
                s_tracker.last_turn_time = now;
                if (s_tracker.cfg.on_status)
                    s_tracker.cfg.on_status("追踪中...", s_tracker.cfg.user_data);
                ESP_LOGI(TAG, "[FaceTrack] Correction done -> TRACKING");
                break;
            }

            if (!s_tracker.last_turn_time ||
                (now - s_tracker.last_turn_time) > FACE_TRACK_TRACK_INTERVAL_MS) {
                priv_send_turn(dx);
            }
            break;

        case FACE_TRACK_STATE_TRACKING:
            if (!has_face) {
                if ((now - s_tracker.last_face_seen_time) > FACE_TRACK_LOST_TIMEOUT_MS) {
                    s_tracker.state = FACE_TRACK_STATE_LOST;
                    app_motor_stop();
                    if (s_tracker.cfg.on_status)
                        s_tracker.cfg.on_status("人脸丢失", s_tracker.cfg.user_data);
                    ESP_LOGW(TAG, "[FaceTrack] Face lost");
                }
                break;
            }

            s_tracker.last_face_seen_time = now;

            if (abs_dx <= FACE_TRACK_DEAD_ZONE && !s_tracker.continuous) {
                if (s_tracker.cfg.on_aligned)
                    s_tracker.cfg.on_aligned(s_tracker.cfg.user_data);

                app_event_t evt = { .type = APP_EVT_FACE_ALIGNED };
                app_ui_logic_post(&evt, false);

                face_tracker_enable(false, false);
                break;
            }

            if ((now - s_tracker.last_turn_time) > FACE_TRACK_TRACK_INTERVAL_MS &&
                abs_dx > FACE_TRACK_DEAD_ZONE) {
                priv_send_turn(dx);
            }
            break;

        case FACE_TRACK_STATE_LOST:
            if (has_face) {
                s_tracker.last_face_seen_time = now;
                s_tracker.state               = FACE_TRACK_STATE_CORRECTING;
                s_tracker.correct_start_time  = now;
                priv_send_turn(dx);
                ESP_LOGI(TAG, "[FaceTrack] Face re-acquired");
            }
            break;

        case FACE_TRACK_STATE_IDLE:
        default:
            break;
    }
}

//====================================================
// 获取状态接口
//====================================================
face_track_state_t face_tracker_get_state(void)
{
    return s_tracker.state;
}

bool face_tracker_is_enabled(void)
{
    return s_tracker.enabled;
}

//====================================================
// 推送人脸检测结果（跨页面追踪用）
//====================================================
bool face_tracker_push_result(const vision_result_t *result)
{
    if (!s_tracker.initialized || !result) return false;

    uint32_t now = lv_tick_get();
    memcpy(&s_tracker.external_face, result, sizeof(vision_result_t));
    s_tracker.external_face_valid = true;
    s_tracker.external_face_time  = now;

    return true;
}