#include "dooi_display.h"
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "app_ui.h"
#include "assets/lang_config.h"

#include "app_vision.h"
#include "app_ui_logic.h"
#include "app_led.h"
#include "esp_timer.h"

#include "app_behavior.h"
#include "app_behavior_types.h"

#define TAG "DooiDisplay"

static const char* k_emotion_to_scene(const char* emotion) {
    static const struct {
        const char* key;
        const char* scene;   /* nullptr 表示不播放 */
    } k_map[] = {
        {"neutral",     nullptr},              /* 默认中性，无需播放场景 */
        {"happy",       "emotion_happy"},
        {"laughing",    "emotion_happy"},
        {"funny",       "emotion_happy"},
        {"sad",         "emotion_sad"},
        {"angry",       "emotion_angry"},
        {"crying",      "emotion_sad"},
        {"loving",      "emotion_love"},
        {"embarrassed", "emotion_shy"},
        {"surprised",   "emotion_surprised"},
        {"shocked",     "emotion_surprised"},
        {"thinking",    "emotion_thinking"},
        {"winking",     "emotion_shy"},        /* 无 winking 场景，复用 shy */
        {"cool",        "emotion_proud"},      /* 无 cool 场景，复用 proud */
        {"relaxed",     "emotion_tired"},      /* 无 relaxed 场景，复用 tired */
        {"delicious",   "emotion_shy"},        /* 无 delicious 场景，复用 shy */
        {"kissy",       "emotion_kiss"},
        {"confident",   "emotion_proud"},      /* 无 confident 场景，复用 proud */
        {"sleepy",      "emotion_sleep"},
        {"silly",       "emotion_curious"},    /* 无 silly 场景，复用 curious */
        {"confused",    "emotion_curious"},    /* 无 confused 场景，复用 curious */
    };
    if (!emotion) return nullptr;
    for (size_t i = 0; i < sizeof(k_map) / sizeof(k_map[0]); i++) {
        if (strcmp(emotion, k_map[i].key) == 0) {
            return k_map[i].scene;
        }
    }
    return nullptr; /* 未列出：未知情绪，不触发播放 */
}

extern "C" {
    void screen_show_wakeup_ack(const char* topic, const char* notification, int timeout_ticks);
}

bool DooiDisplay::s_skip_next_emotion = false;

DooiDisplay::DooiDisplay() {
    ESP_LOGI(TAG, "DooiDisplay initialized");
}

DooiDisplay::~DooiDisplay() {
}

void DooiDisplay::ArmSkipNextEmotion(void) {
    s_skip_next_emotion = true;
    ESP_LOGI(TAG, "ArmSkipNextEmotion: 下一次 SetEmotion 将被跳过");
}

bool DooiDisplay::IsSkipNextEmotionArmed(void) {
    return s_skip_next_emotion;
}

bool DooiDisplay::Lock(int timeout_ms) {
    return true;
}

void DooiDisplay::Unlock() {
}

void DooiDisplay::SetStatus(const char* status) {
    ESP_LOGW(TAG, "SetStatus: %s", status);
    if (status && strlen(status) > 0) 
    {
        app_event_t evt = {.type = APP_EVT_NONE};

        if (strcmp(status, Lang::Strings::LISTENING) == 0) {
            evt.type = APP_EVT_STATE_LISTENING;
        } else if (strcmp(status, Lang::Strings::STANDBY) == 0) {
            evt.type = APP_EVT_STATE_STANDBY;
        } else if (strcmp(status, Lang::Strings::SPEAKING) == 0) {
            evt.type = APP_EVT_STATE_SPEAKING;
        } else if (strcmp(status, Lang::Strings::CONNECTING) == 0) {
            evt.type = APP_EVT_STATE_CONNECTING;
        }  else if(strcmp(status, Lang::Strings::LOADING_PROTOCOL) == 0){
            evt.type = APP_EVT_STATE_LOADING_PROTOCOL;
        }else if(strcmp(status, Lang::Strings::ACTIVATION) == 0){
            evt.type = APP_EVT_STATE_ACTIVATION;
        }else if(strcmp(status, Lang::Strings::CHECKING_NEW_VERSION) == 0){
            evt.type = APP_EVT_STATE_CHECKING_NEW_VERSION;
        }else if (strcmp(status, Lang::Strings::ERROR) == 0) {
            evt.type = APP_EVT_STATE_ERROR;
        }

        app_ui_logic_post(&evt, false);
    }
}

void DooiDisplay::ShowNotification(const std::string &notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void DooiDisplay::ShowNotification(const char* notification, int duration_ms) {
    ESP_LOGW(TAG, "ShowNotification: %s", notification);

    if(strcmp(notification, Lang::Strings::SCANNING_WIFI) == 0)
    {
        app_event_t evt = {.type = APP_EVT_NONE};
        evt.type = APP_EVT_STATE_CONNECT_WIFI;
        app_ui_logic_post(&evt, false);
    }

    if(app_ui_logic_get_state() != APP_EVT_STATE_LOADING_PROTOCOL)
    {
        if(strncmp(notification, "连接", 4) == 0)
        {
            return;
        }
        screen_show_wakeup_ack("SYS MESSAGE", notification, APP_UI_MS_TO_TICKS(10000));
    }
}

void DooiDisplay::UpdateStatusBar(bool update_all) {
}

void DooiDisplay::SetEmotion(const char* emotion) {
    ESP_LOGW(TAG, "SetEmotion: %s", emotion);
    if (!emotion || !emotion[0]) return;

    /* compose 自主编排动作执行后，下一次 SetEmotion 应被跳过，避免打断该动作 */
    if (s_skip_next_emotion) {
        s_skip_next_emotion = false;
        ESP_LOGW(TAG, "SetEmotion: 已跳过本次表情（compose 动作保护），emotion=%s", emotion);
        return;
    }

    const char* scene = k_emotion_to_scene(emotion);
    if (scene && scene[0]) {
        ESP_LOGI(TAG, "SetEmotion '%s' -> play scene '%s'", emotion, scene);
        app_ui_logic_request_behavior(scene, nullptr, BEHAVIOR_PRIO_DEFAULT);
    }
}

void DooiDisplay::SetChatMessage(const char* role, const char* content) {
    if(strcmp(role, "user") == 0)
    {
        app_behavior_play_scene("action_chat_user", nullptr, BEHAVIOR_PRIO_DEFAULT);
    }
    
    app_event_type_t env = app_ui_logic_get_state();
    ESP_LOGW(TAG, "SetChatMessage environment: %d", env);

    if(env == APP_EVT_STATE_ACTIVATION || env == APP_EVT_STATE_CHECKING_NEW_VERSION || \
       env == APP_EVT_STATE_LOADING_PROTOCOL || env == APP_EVT_STATE_CONNECT_WIFI)
    {
        char ip_buf[64] = {0};
        char buf[128] = {0};
        get_wifi_ip_string(ip_buf, sizeof(ip_buf));
        sprintf(buf, "%s\n%s", content, ip_buf);

        screen_show_wakeup_ack("SYS MESSAGE", buf, APP_UI_MS_TO_TICKS(1500));
    }
    ESP_LOGW(TAG, "Role:%s", role);
    ESP_LOGW(TAG, "     %s", content);
}

void DooiDisplay::SetTheme(Theme* theme) {
    ESP_LOGI(TAG, "Theme changed to: %s", theme->name().c_str());
}

void DooiDisplay::SetPowerSaveMode(bool on) {
    ESP_LOGW(TAG, "SetPowerSaveMode: %d", on);
}


void DooiDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    ESP_LOGI(TAG, "SetPreviewImage called with image size: %d bytes", image->image_dsc()->data_size);
}