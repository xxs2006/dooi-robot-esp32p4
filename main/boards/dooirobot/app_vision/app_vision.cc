#include "app_vision.h"

#include "dl_image.hpp"
#include "hand_detect.hpp"
#include "hand_gesture_recognition.hpp"
#include "human_face_detect.hpp"
#include "dooi_camera.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <vector>
#include <memory>
#include <atomic>

#define APP_VISION_TASK_PRIORITY  1

static const char *TAG = "app_vision";

static bool app_vision_first_run = false;

SemaphoreHandle_t g_model_mutex = NULL;

extern "C" 
{
    void app_model_mutex_init(void) {
        if (g_model_mutex == NULL) {
            g_model_mutex = xSemaphoreCreateMutex();
        }
    }
    void app_model_mutex_lock(void) {
        // xSemaphoreTake(g_model_mutex, portMAX_DELAY);
    }
    void app_model_mutex_unlock(void) {
        // xSemaphoreGive(g_model_mutex);
    }
}


/* ================================================================
 *  模型接口
 * ================================================================ */
class IModel {
public:
    virtual ~IModel() = default;
    virtual bool load()   = 0;
    virtual void unload() = 0;
    virtual bool is_loaded() const = 0;
    virtual std::vector<vision_result_t> infer(const dl::image::img_t &img) = 0;
};

/* ── 人脸检测 ── */
class FaceDetectModel : public IModel {
public:
    ~FaceDetectModel() override = default;
    bool load() override { return true; }
    void unload() override {} // 静态实例无需手动卸载
    bool is_loaded() const override { return get_engine() != nullptr; }

    std::vector<vision_result_t> infer(const dl::image::img_t &img) override {
        std::vector<vision_result_t> out;
        auto* engine = get_engine(); // 获取全局唯一静态实例
        if (!engine || img.data == nullptr) return out;

        auto &dets = engine->run(img);
        for (auto &d : dets) {
            out.push_back({"face", d.score, {d.box[0], d.box[1], d.box[2], d.box[3]}});
        }
        return out;
    }

private:
    // 💡 使用静态局部指针，确保全局只有一份，且永不析构
    static HumanFaceDetect* get_engine() {
        static HumanFaceDetect* instance = nullptr;
        if (!instance) {
            ESP_LOGI(TAG, "Initializing static HumanFaceDetect...");
            try {
                instance = new HumanFaceDetect();
            } catch (...) {
                ESP_LOGE(TAG, "Static HumanFaceDetect init failed!");
            }
        }
        return instance;
    }
};

/* ── 手势识别 ── */
class GestureModel : public IModel {
public:
    ~GestureModel() override = default;
    bool load() override { return true; }
    void unload() override {} // 静态实例无需手动卸载
    bool is_loaded() const override { return get_hand() != nullptr && get_gesture() != nullptr; }

    std::vector<vision_result_t> infer(const dl::image::img_t &img) override {
        std::vector<vision_result_t> out;
        auto* hand = get_hand();
        auto* gesture = get_gesture();
        if (!hand || !gesture || img.data == nullptr) return out;

        auto hands = hand->run(img);
        if (hands.empty()) return out;

        auto gestures = gesture->recognize(img, hands);
        for (auto &g : gestures) {
            out.push_back({g.cat_name, g.score, {0, 0, 0, 0}});
        }
        return out;
    }

private:
    // 💡 手部检测全局静态唯一实例
    static HandDetect* get_hand() {
        static HandDetect* instance = nullptr;
        if (!instance) {
            ESP_LOGI(TAG, "Initializing static HandDetect...");
            try {
                instance = new HandDetect();
            } catch (...) {
                ESP_LOGE(TAG, "Static HandDetect init failed!");
            }
        }
        return instance;
    }

    // 💡 手势识别全局静态唯一实例
    static HandGestureRecognizer* get_gesture() {
        static HandGestureRecognizer* instance = nullptr;
        if (!instance) {
            ESP_LOGI(TAG, "Initializing static HandGestureRecognizer...");
            try {
                instance = new HandGestureRecognizer();
            } catch (...) {
                ESP_LOGE(TAG, "Static HandGestureRecognizer init failed!");
            }
        }
        return instance;
    }
};

/* ================================================================
 *  全局回调（独立于运行生命周期）
 * ================================================================ */
static vision_result_cb_t g_callback  = nullptr;
static void              *g_userdata  = nullptr;

/* ================================================================
 *  运行上下文
 * ================================================================ */
struct VisionCtx {
    std::unique_ptr<IModel> face;
    std::unique_ptr<IModel> gesture;

    std::atomic<vision_mode_t> mode{VISION_MODE_FACE_DETECT};
    vision_intervals_t intervals{2000, 2000};
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};

    TaskHandle_t task = nullptr;
    TaskHandle_t notifyTo  = nullptr;

    uint32_t get_interval(vision_mode_t m) const {
        switch (m) {
        case VISION_MODE_FACE_DETECT:       return intervals.face_detect_ms;
        case VISION_MODE_GESTURE_RECOGNIZE: return intervals.gesture_recognize_ms;
        default:                            return 2000;
        }
    }

    IModel *model_for(vision_mode_t m) {
        if (m == VISION_MODE_FACE_DETECT)       return face.get();
        if (m == VISION_MODE_GESTURE_RECOGNIZE) return gesture.get();
        return nullptr;
    }

    bool ensure_loaded(vision_mode_t m) {
        IModel *mdl = model_for(m);
        return mdl && (mdl->is_loaded() || mdl->load());
    }

    bool grab(vision_mode_t m, dl::image::img_t &img) {
        FrameBuffer fb;
        if (m == VISION_MODE_FACE_DETECT) {
            fb = camera_get_face_frame();
        } else if (m == VISION_MODE_GESTURE_RECOGNIZE) {
            fb = camera_get_gesture_frame();
        } else {
            return false;
        }

        if (!fb.data) return false;
        img.data     = fb.data;
        img.width    = fb.width;
        img.height   = fb.height;
        img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;
        return true;
    }

    void run_once(vision_mode_t m) {
        if (!ensure_loaded(m)) return;

        dl::image::img_t img;
        if (!grab(m, img)) return;

        auto results = std::vector<vision_result_t>();
        app_model_mutex_lock();
        results = model_for(m)->infer(img);
        app_model_mutex_unlock();

        if (!results.empty() && g_callback) {
            g_callback(m, results.data(), results.size(), g_userdata);
        }
    }

    void unload_all() {
        // 确保在释放模型时，抢占全局锁，防止其他地方（如 infer）同时在使用或释放
        if (g_model_mutex) {
            xSemaphoreTake(g_model_mutex, portMAX_DELAY);
        }
        
        // 释放前先置为空指针，防范二次进入
        if (face)    { face->unload();    face.reset(); }
        if (gesture) { gesture->unload(); gesture.reset(); }

        if (g_model_mutex) {
            xSemaphoreGive(g_model_mutex);
        }
    }
};


static VisionCtx *g_ctx = nullptr;

/* ================================================================
 *  任务主循环
 * ================================================================ */
static void vision_task(void *arg)
{
    auto *ctx = static_cast<VisionCtx *>(arg);
    ESP_LOGI(TAG, "Vision task started");

    if (app_vision_first_run) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        app_vision_first_run = false;
    }

    // Round-robin: 每种模式独立频率执行
    TickType_t last_face = xTaskGetTickCount();
    TickType_t last_gesture = xTaskGetTickCount();

    while (ctx->running.load()) {
        vision_mode_t cur = ctx->mode.load();

        if (cur == VISION_MODE_ROUND_ROBIN) {
            TickType_t now = xTaskGetTickCount();

            // Face: 每 face_detect_ms 执行一次
            if (now - last_face >= pdMS_TO_TICKS(ctx->intervals.face_detect_ms)) {
                ctx->run_once(VISION_MODE_FACE_DETECT);
                last_face = now;
            }

            // Gesture: 每 gesture_recognize_ms 执行一次
            if (now - last_gesture >= pdMS_TO_TICKS(ctx->intervals.gesture_recognize_ms)) {
                ctx->run_once(VISION_MODE_GESTURE_RECOGNIZE);
                last_gesture = now;
            }

            // 短暂休眠，避免过度占用 CPU（使用最小的间隔作为检查周期）
            uint32_t min_interval = ctx->intervals.face_detect_ms;
            if (ctx->intervals.gesture_recognize_ms < min_interval) {
                min_interval = ctx->intervals.gesture_recognize_ms;
            }
            uint32_t sleep_ms = min_interval > 5 ? 5 : min_interval;
            vTaskDelay(pdMS_TO_TICKS(sleep_ms));
        } else {
            ctx->run_once(cur);
            uint32_t ms = ctx->get_interval(cur);
            uint32_t remain = ms;
            while (ctx->running.load() && remain > 0) {
                uint32_t slice = remain > 10 ? 10 : remain;
                vTaskDelay(pdMS_TO_TICKS(slice));
                remain -= slice;
            }
        }
    }

    ESP_LOGI(TAG, "Vision task exited");

    ctx->unload_all();

    TaskHandle_t notify = ctx->notifyTo;
    if (notify) {
        xTaskNotifyGive(notify);
    }

    vTaskDelete(nullptr);
}

/* ================================================================
 *  C API
 * ================================================================ */

/* 与模型输出保持一致的标签表，顺序对应 gesture_type_t */
static const char *const s_gesture_labels[GESTURE_COUNT] = {
    [GESTURE_ONE]        = "one",
    [GESTURE_TWO]        = "two",
    [GESTURE_THREE]      = "three",
    [GESTURE_FOUR]       = "four",
    [GESTURE_FIVE]       = "five",
    [GESTURE_LIKE]       = "like",
    [GESTURE_OK]         = "ok",
    [GESTURE_NO_GESTURE] = "no_gesture",
    [GESTURE_CALL]       = "call",
    [GESTURE_DISLIKE]    = "dislike",
    [GESTURE_NO_HAND]    = "no_hand",
    [GESTURE_UNKNOWN]    = "unknown",
};

extern "C" {

void app_vision_set_callback(vision_result_cb_t cb, void *user_data)
{
    g_callback = cb;
    g_userdata = user_data;
    ESP_LOGI(TAG, "Callback %s", cb ? "registered" : "cleared");
}

bool app_vision_start(vision_mode_t mode, const vision_intervals_t *intervals)
{
    if (mode >= VISION_MODE_MAX) return false;

    vision_intervals_t iv = {2000, 2000};
    if (intervals) {
        iv = *intervals;
    }

    if (g_ctx) {
        if (g_ctx->running.load()) {
            g_ctx->mode.store(mode);
            g_ctx->intervals = iv;
            ESP_LOGI(TAG, "Updated: mode=%d intervals={face=%lu,gesture=%lu}", 
                     mode, (unsigned long)iv.face_detect_ms, 
                     (unsigned long)iv.gesture_recognize_ms);
            return true;
        }

        if (g_ctx->stopping.load()) {
            ESP_LOGW(TAG, "Vision is stopping, start rejected");
            return false;
        }
    }

    if (!g_ctx) {
        g_ctx = new VisionCtx();
        g_ctx->face    = std::make_unique<FaceDetectModel>();
        g_ctx->gesture = std::make_unique<GestureModel>();
        ESP_LOGI(TAG, "Vision system initialized");
    }

    g_ctx->mode.store(mode);
    g_ctx->intervals = iv;
    g_ctx->running.store(true);
    g_ctx->stopping.store(false);

    xTaskCreatePinnedToCore(vision_task, "vision", 12 * 1024, g_ctx, APP_VISION_TASK_PRIORITY, &g_ctx->task, 1);

    ESP_LOGI(TAG, "Started: mode=%d intervals={face=%lu,gesture=%lu} ms", 
             mode, (unsigned long)iv.face_detect_ms, 
             (unsigned long)iv.gesture_recognize_ms);
    return true;
}

void app_vision_stop(void)
{
    VisionCtx *ctx = g_ctx;
    if (!ctx) return;

    bool expected = false;
    if (!ctx->stopping.compare_exchange_strong(expected, true)) {
        return;
    }

    ctx->notifyTo = xTaskGetCurrentTaskHandle();
    ctx->running.store(false);
    ESP_LOGI(TAG, "Stop requested, waiting...");

    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000)) == 0) {
        ESP_LOGE(TAG, "Vision task stop timeout");
        return;
    }

    if (g_ctx == ctx) {
        g_ctx = nullptr;
    }

    delete ctx;

    ESP_LOGI(TAG, "Vision task confirmed stopped");
}

bool app_vision_switch_mode(vision_mode_t mode)
{
    if (!g_ctx || mode >= VISION_MODE_MAX) return false;
    g_ctx->mode.store(mode);
    ESP_LOGI(TAG, "Mode → %d", mode);
    return true;
}

void app_vision_set_interval(uint32_t interval_ms)
{
    if (!g_ctx || !interval_ms) return;
    g_ctx->intervals.face_detect_ms = interval_ms;
    g_ctx->intervals.gesture_recognize_ms = interval_ms;
    ESP_LOGI(TAG, "Intervals → all = %lu ms", (unsigned long)interval_ms);
}

gesture_type_t gesture_type_from_label(const char *label)
{
    if (!label) return GESTURE_UNKNOWN;

    for (int i = 0; i < GESTURE_COUNT; i++) {
        if (s_gesture_labels[i] && strcmp(label, s_gesture_labels[i]) == 0) {
            return (gesture_type_t)i;
        }
    }
    return GESTURE_UNKNOWN;
}

const char *gesture_type_to_str(gesture_type_t type)
{
    if (type < GESTURE_COUNT && s_gesture_labels[type]) {
        return s_gesture_labels[type];
    }
    return "unknown";
}

void app_vision_priority_boost(int task_priority)
{
    if (g_ctx && g_ctx->task) {
        vTaskPrioritySet(g_ctx->task, task_priority);
    }
}

void app_vision_priority_restore(void)
{
    if (g_ctx && g_ctx->task) {
        vTaskPrioritySet(g_ctx->task, APP_VISION_TASK_PRIORITY);
    }
}

} /* extern "C" */