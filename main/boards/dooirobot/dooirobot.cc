#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/display.h"

#include "esp_video_init.h"
#include "esp_cam_sensor_xclk.h"

#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"

#include "esp_ldo_regulator.h" 
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>

#include "dooi_audio_codec.h"
#include "dooi_camera.h"
#include "dooi_display.h"
#include "app_ui.h"
#include "app_periphral.h"
#include "app_ble_control.h"
#include "app_webserver.h"
#include "app_mcp_tool.h"
#include "dooi_nvs.h"
#include "app_ui_logic.h"
#include "app_behavior.h"
#include "app_behavior_fs.h"
#include "app_behavior_status.h"
#include "esp_private/brownout.h"
#include "esp_wifi.h"
#include "ssid_manager.h"


#define TAG "DooiRobotBoard"

/* I2C0 总线句柄 挂多个设备 */
i2c_master_bus_handle_t i2c_bus_;

class WifiBoard;
static WifiBoard* g_board = nullptr;
extern "C" void dooi_enter_wifi_config_mode(void)
{
    // esp_wifi_restore();
    if (g_board) {
        g_board->EnterWifiConfigMode();
    } else {
        ESP_LOGW(TAG, "board not ready");
    }
}

extern "C" void dooi_set_volume(int volume)
{
    if (g_board == nullptr) {
        ESP_LOGW(TAG, "dooi_set_volume: board not ready, drop volume=%d", volume);
        return;
    }
    g_board->GetAudioCodec()->SetOutputVolume(volume);
}

/* 供 dooi_nvs.c（C 侧）调用：把 WiFi 凭证成对交给 SsidManager 持久化。
 * 重启后 WifiStation 从 SsidManager 取凭证连接，不能再手写 "wifi"/"ssid"。 */
extern "C" esp_err_t dooi_wifi_store_credentials(const char *ssid, const char *password)
{
    if (ssid == nullptr || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    SsidManager::GetInstance().AddSsid(ssid, password != nullptr ? password : "");
    ESP_LOGI(TAG, "Wi-Fi credentials saved via SsidManager: %s", ssid);
    return ESP_OK;
}

/* 供 dooi_nvs.c（C 侧）调用：取最新一次配置的 SSID（SsidManager 列表末位） */
extern "C" esp_err_t dooi_wifi_get_ssid(char *out, size_t max_len)
{
    if (out == nullptr || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    const auto &list = SsidManager::GetInstance().GetSsidList();
    if (list.empty()) {
        return ESP_ERR_NOT_FOUND;
    }
    /* SsidManager::AddSsid 把新条目插入列表头部，故 front() 才是最新一次配置 */
    const std::string &ssid = list.front().ssid;
    if (ssid.empty() || ssid.size() >= max_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, ssid.c_str(), ssid.size() + 1);
    return ESP_OK;
}

/* 供 dooi_nvs.c（C 侧）调用：清空所有 WiFi 凭证（工厂重置用） */
extern "C" void dooi_wifi_clear_credentials(void)
{
    SsidManager::GetInstance().Clear();
    ESP_LOGW(TAG, "Wi-Fi credentials cleared via SsidManager");
}

class DooiRobotBoard : public WifiBoard {
private:
    Button boot_button_;
    DooiCamera* camera_ = nullptr;
    Display* display_ = new DooiDisplay();
    PwmBacklight* backlight_ = nullptr;

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeCamera() {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << CAMERA_PIN_PWDN,
            .mode = GPIO_MODE_OUTPUT,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&io));
        vTaskDelay(pdMS_TO_TICKS(10)); 
        gpio_set_level(CAMERA_PIN_PWDN, 1);

        vTaskDelay(pdMS_TO_TICKS(100));      // 等待摄像头上电稳定

        esp_video_init_csi_config_t base_csi_config = {
            .sccb_config = {
                .init_sccb = false,
                .i2c_handle = i2c_bus_,
                .freq = 100000,
            },
            .reset_pin = GPIO_NUM_NC,
            .pwdn_pin  = GPIO_NUM_NC,
        };

        esp_video_init_config_t cam_config = {
            .csi = &base_csi_config,
        };

        camera_ = new DooiCamera(cam_config);
    }

    esp_err_t bsp_enable_sdio_power(void)
    {
        static esp_ldo_channel_handle_t sdio_chan = NULL;
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id = 4,
            .voltage_mv = 3300,
        };
        esp_ldo_acquire_channel(&ldo_cfg, &sdio_chan);
        ESP_LOGI(TAG, "SDIO Powered on");

        return ESP_OK;
    }

    esp_err_t bsp_enable_mipi_power(void) 
    {
        // Turn on the power for MIPI, so it can go from "No Power" state to "Shutdown" state
        static esp_ldo_channel_handle_t mipi_chan = NULL;
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id = 3,
            .voltage_mv = 2500,
        };
        esp_ldo_acquire_channel(&ldo_cfg, &mipi_chan);
        ESP_LOGI(TAG, "Mipi Powered on");

        return ESP_OK;
    }

public:
    DooiRobotBoard() : boot_button_(BOOT_BUTTON_GPIO)
    {
        g_board = this;

        /* 参数存储最先初始化：空板（首次烧录）在此把默认值落盘，
         * 后续所有子系统的 dooi_nvs_get_* 都能拿到确定值。 */
        dooi_nvs_init();

        bsp_enable_sdio_power(); // 开启SDIO电源，否则wifi无法正常工作； For gpio 组电压
        bsp_enable_mipi_power(); // 开启Mipi电源，否则Mipi无法正常工作； For Mipi 组电压

        backlight_ = new PwmBacklight(LCD_PIN_BL, false);
        // backlight_->RestoreBrightness();
        backlight_->SetBrightness(40, false);


        InitializeI2c();
        InitializeButtons();
        InitializeCamera();

#if 1
        esp_log_level_set("AFE", ESP_LOG_ERROR); 
        app_model_mutex_init();   // 模型互斥资源；FIXME：暂时用互斥规避
#endif

        app_mcp_tool_register();
        app_periphral_init();
        app_ui_init();
        app_ui_logic_init();

        /* 行为系统：先挂载资源文件系统，再启动管理器与状态管理 */
        app_behavior_init();
        app_behavior_status_init();
 
        // app_ble_control_start();
        // esp_brownout_disable();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static DooiAudioCodec audio_codec(
            i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7243E_ADDR,
            AUDIO_INPUT_REFERENCE);

        /* 首次取用时恢复持久化音量。放在这里而不是 dooi_nvs 里：
         *  NVS 层不应反向依赖音频层，且此处 I2C/编解码器一定已就绪。 */
        static bool s_volume_restored = false;
        if (!s_volume_restored) {
            s_volume_restored = true;
            int volume = 70;
            dooi_nvs_get_volume(&volume);
            audio_codec.SetOutputVolume(volume);
            ESP_LOGI(TAG, "Audio volume restored: %d", volume);
        }

        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }

    virtual Backlight* GetBacklight() override
    {
        return backlight_;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = power_manager_is_charging();
        discharging = !charging;
        level = power_manager_get_level();
        return true;
    }
};

DECLARE_BOARD(DooiRobotBoard);