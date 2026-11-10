# xiaozhi-esp32 WiFi 连接改动文档

> 适用版本：`v2.2.3`（commit `b34a9b19`）
> 适用硬件：ESP32-P4 + ESP32-C6（ESP-Hosted 架构）
> 修改时间：2026-07-24
> 修改组件：`managed_components/78__esp-wifi-connect`

---

## 原因分析

问题不在 AP 本身，手机能连上 `Xiaozhi-XXXX` 且配网页面能打开，问题是 **AP 模式下扫描不到周围 WiFi 列表**，以及后续 **AP → STA 切换时崩溃 / 连接超时**。

ESP32-P4 本身没有 WiFi/Bluetooth，项目通过 SDIO 把 ESP32-C6 当作协处理器跑 `ESP-Hosted`，P4 侧调用 `esp_wifi_*` 实际都是 RPC 请求。C6 协处理器响应有延迟，且日志中出现：

```
W (2496) transport: Version mismatch: Host [2.11.0] > Co-proc [0.0.0]
```

说明 C6 侧版本信息异常，RPC 往返更慢。原代码按 ESP32-S3 等集成 WiFi 芯片的时序设计，在 P4+C6 上会出现以下三类问题。

---

## 问题 1：AP 模式下扫描不到周围 WiFi 列表

### 根因

`wifi_configuration_ap.cc` 的 `Start()` 在 AP 启动后立即调用 `esp_wifi_scan_start()`。此时 C6 协处理器可能尚未就绪，RPC 返回 `ESP_ERR_WIFI_STATE(12294)`，`SCAN_DONE` 事件永不触发，周期扫描定时器也不会启动，导致 WiFi 列表永远为空。

### 修改文件

- `managed_components/78__esp-wifi-connect/wifi_configuration_ap.cc`
- `managed_components/78__esp-wifi-connect/wifi_station.cc`

### 1.1 `wifi_configuration_ap.cc` — `Start()` 延迟首次扫描 + 失败重试

**改动前：**

```cpp
// Start scan immediately
esp_wifi_scan_start(nullptr, false);

// Setup periodic WiFi scan timer
esp_timer_create_args_t timer_args = {
    .callback = [](void* arg) {
        auto* self = static_cast<WifiConfigurationAp*>(arg);
        if (!self->is_connecting_) {
            esp_wifi_scan_start(nullptr, false);
        }
    },
    .arg = this,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "wifi_scan_timer",
    .skip_unhandled_events = true
};
ESP_ERROR_CHECK(esp_timer_create(&timer_args, &scan_timer_));
```

**改动后：**

```cpp
// Setup periodic WiFi scan timer
esp_timer_create_args_t timer_args = {
    .callback = [](void* arg) {
        auto* self = static_cast<WifiConfigurationAp*>(arg);
        if (!self->is_connecting_) {
            esp_err_t ret = esp_wifi_scan_start(nullptr, false);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Scan start failed: %s, retrying in 3s", esp_err_to_name(ret));
                esp_timer_start_once(self->scan_timer_, 3 * 1000000);
            }
        }
    },
    .arg = this,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "wifi_scan_timer",
    .skip_unhandled_events = true
};
ESP_ERROR_CHECK(esp_timer_create(&timer_args, &scan_timer_));

// Delay first scan by 2s to allow co-processor (ESP32-C6) to be ready.
// On ESP32-P4 + C6 ESP-Hosted architecture, WiFi runs on C6 via SDIO RPC
// and may not be immediately ready after esp_wifi_start().
esp_timer_start_once(scan_timer_, 2 * 1000000);
```

**说明：**

- 删除“立即扫描”，改为定时器 2 秒后首次扫描。
- 定时器回调检查 `esp_wifi_scan_start()` 返回值，失败时日志告警并 3 秒后重试。

### 1.2 `wifi_station.cc` — `Start()` 定时器前移 + STA_START 延迟扫描

**改动前：**

```cpp
ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
ESP_ERROR_CHECK(esp_wifi_start());

if (max_tx_power_ != 0) {
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(max_tx_power_));
}

// Setup the timer to scan WiFi
esp_timer_create_args_t timer_args = {
    .callback = [](void* arg) {
        esp_wifi_scan_start(nullptr, false);
    },
    .arg = this,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "WiFiScanTimer",
    .skip_unhandled_events = true
};
ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
```

**改动后：**

```cpp
ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

// Create scan timer BEFORE esp_wifi_start() so timer_handle_ is valid
// when WIFI_EVENT_STA_START fires. On ESP32-P4 + C6 ESP-Hosted, the
// STA_START event may fire before this function returns.
esp_timer_create_args_t timer_args = {
    .callback = [](void* arg) {
        auto* self = static_cast<WifiStation*>(arg);
        esp_err_t ret = esp_wifi_scan_start(nullptr, false);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Scan start failed: %s, retrying in 3s", esp_err_to_name(ret));
            esp_timer_start_once(self->timer_handle_, 3 * 1000000);
        }
    },
    .arg = this,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "WiFiScanTimer",
    .skip_unhandled_events = true
};
ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));

ESP_ERROR_CHECK(esp_wifi_start());

if (max_tx_power_ != 0) {
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(max_tx_power_));
}
```

**说明：**

- 将定时器创建移到 `esp_wifi_start()` 之前，避免 `WIFI_EVENT_STA_START` 事件到达时 `timer_handle_` 还是 `nullptr`。
- 回调增加失败重试逻辑。

### 1.3 `wifi_station.cc` — `WifiEventHandler` STA_START 延迟 2 秒扫描

**改动前：**

```cpp
if (event_id == WIFI_EVENT_STA_START) {
    esp_wifi_scan_start(nullptr, false);
    if (this_->on_scan_begin_) {
        this_->on_scan_begin_();
    }
}
```

**改动后：**

```cpp
if (event_id == WIFI_EVENT_STA_START) {
    // Delay scan by 2s for ESP32-P4 + C6 ESP-Hosted to allow co-processor
    // to be ready. On integrated WiFi chips this adds minimal latency.
    // The timer callback also retries on failure (3s interval).
    esp_timer_start_once(this_->timer_handle_, 2 * 1000000);
    if (this_->on_scan_begin_) {
        this_->on_scan_begin_();
    }
}
```

**说明：**

- STA 启动事件中也改为 2 秒延迟扫描，与 AP 模式保持一致。

---

## 问题 2：AP → STA 切换时 `netif` 重复键崩溃

### 根因

APSTA 模式启动时，`esp_wifi_start()` 会隐式创建一个 `WIFI_STA_DEF` netif。`WifiConfigurationAp::Stop()` 只销毁了显式创建的 AP netif，没有清理这个隐式 STA netif。随后 `WifiStation::Start()` 调用 `esp_netif_create_default_wifi_sta()` 时，因为 `WIFI_STA_DEF` 键已存在，触发 assert：

```
E (3175) esp_netif_lwip: esp_netif_new_api: Failed to configure netif ... duplicate key
assert failed: esp_netif_create_default_wifi_sta wifi_default.c:422 (netif)
```

此外，`esp_wifi_stop()` 在 P4+C6 上是异步 RPC，返回后 C6 可能还没真正停止 WiFi，立即创建新 netif 也容易冲突。

### 修改文件

- `managed_components/78__esp-wifi-connect/wifi_configuration_ap.cc`
- `managed_components/78__esp-wifi-connect/wifi_manager.cc`

### 2.1 `wifi_configuration_ap.cc` — `Stop()` 加延迟并销毁隐式 STA netif

**改动前：**

```cpp
// 停止WiFi（但不 deinit，WiFi 驱动由 WifiManager 管理）
esp_wifi_stop();

// 销毁网络接口
if (ap_netif_) {
    esp_netif_destroy_default_wifi(ap_netif_);
    ap_netif_ = nullptr;
}

ESP_LOGI(TAG, "Wifi configuration AP stopped");
```

**改动后：**

```cpp
// 停止WiFi（但不 deinit，WiFi 驱动由 WifiManager 管理）
esp_wifi_stop();

// On ESP32-P4 + C6 ESP-Hosted, esp_wifi_stop() is an async RPC call.
// Wait for C6 to fully stop WiFi before destroying netif, otherwise
// the implicit STA netif from APSTA mode may not be cleaned up.
vTaskDelay(pdMS_TO_TICKS(500));

// 销毁AP网络接口
if (ap_netif_) {
    esp_netif_destroy_default_wifi(ap_netif_);
    ap_netif_ = nullptr;
}

// APSTA mode implicitly creates a STA netif (WIFI_STA_DEF).
// Destroy it too, otherwise esp_netif_create_default_wifi_sta() in
// WifiStation::Start() will assert with "duplicate key" crash.
esp_netif_t* implicit_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
if (implicit_sta_netif) {
    esp_netif_destroy_default_wifi(implicit_sta_netif);
    ESP_LOGI(TAG, "Destroyed implicit STA netif from APSTA mode");
}

ESP_LOGI(TAG, "Wifi configuration AP stopped");
```

**说明：**

- `esp_wifi_stop()` 后等待 500ms，让 C6 异步清理完成。
- 通过 `esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")` 查找并销毁 APSTA 模式遗留的 STA netif。

### 2.2 `wifi_manager.cc` — 切换模式时加延迟

**新增头文件：**

```cpp
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
```

**`StartStation()` 改动后：**

```cpp
// Auto-stop config AP if active
if (config_mode_active_) {
    ESP_LOGI(TAG, "Stopping config AP before starting station");
    config_ap_->Stop();
    config_mode_active_ = false;
    // Notify outside lock
    mutex_.unlock();
    NotifyEvent(WifiEvent::ConfigModeExit);
    // Delay for co-processor to fully release WiFi resources after APSTA stop.
    // On P4+C6 ESP-Hosted, esp_wifi_stop() is async and netif cleanup may lag.
    vTaskDelay(pdMS_TO_TICKS(500));
    mutex_.lock();
}
```

**`StartConfigAp()` 改动后：**

```cpp
// Auto-stop station if active
if (station_active_) {
    ESP_LOGI(TAG, "Stopping station before starting config AP");
    station_->Stop();
    station_active_ = false;
    mutex_.unlock();
    NotifyEvent(WifiEvent::Disconnected);
    // Delay for co-processor cleanup before starting AP mode
    vTaskDelay(pdMS_TO_TICKS(500));
    mutex_.lock();
}
```

**说明：**

- AP → STA 和 STA → AP 两种切换都增加 500ms 延迟，给 C6 协处理器留出清理/创建 netif 的时间。

---

## 问题 3：ConnectToWifi 连接超时

### 根因

用户在配网页面选择 WiFi 并输入密码后，页面提示 `Failed to connect to the Access Point`。日志显示：

```
I (43041) WifiConfigurationAp: Connecting to WiFi CMCC-2106
...
E (53041) WifiConfigurationAp: Failed to connect to WiFi CMCC-2106
```

原代码 2.4G 只等待 10 秒，而 P4+C6 ESP-Hosted 因协处理器版本不匹配、RPC 延迟高，连接事件可能 10 秒内回不来。另外 `esp_wifi_set_config()` 到 `esp_wifi_connect()` 之间没有给 RPC 传播留时间。

### 修改文件

- `managed_components/78__esp-wifi-connect/wifi_configuration_ap.cc`

### 3.1 `ConnectToWifi()` 延长超时 + 重试

**改动前：**

```cpp
ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
auto ret = esp_wifi_connect();
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to connect to WiFi: %d", ret);
    is_connecting_ = false;
    return false;
}
ESP_LOGI(TAG, "Connecting to WiFi %s", ssid.c_str());

// Wait for the connection to complete for 10 or 25 seconds
EventBits_t bits = xEventGroupWaitBits(
    event_group_,
    WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
    pdTRUE,
    pdFALSE,
#ifdef CONFIG_SOC_WIFI_SUPPORT_5G
    pdMS_TO_TICKS(25000)
#else
    pdMS_TO_TICKS(10000)
#endif
);
is_connecting_ = false;

if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "Connected to WiFi %s", ssid.c_str());
    esp_wifi_disconnect();
    return true;
} else {
    ESP_LOGE(TAG, "Failed to connect to WiFi %s", ssid.c_str());
    return false;
}
```

**改动后：**

```cpp
ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
// On ESP32-P4 + C6 ESP-Hosted, allow time for RPC config to propagate to C6
vTaskDelay(pdMS_TO_TICKS(200));

// Try connecting with retry. On P4+C6 ESP-Hosted with co-processor
// version mismatch, RPC round-trips are slow and the first attempt may
// time out. Use 25s timeout for both 2.4G and 5G to accommodate the
// extra latency.
const int max_attempts = 2;
for (int attempt = 1; attempt <= max_attempts; attempt++) {
    xEventGroupClearBits(event_group_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    auto ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect() failed: %s (attempt %d/%d)",
                 esp_err_to_name(ret), attempt, max_attempts);
        if (attempt < max_attempts) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        is_connecting_ = false;
        return false;
    }
    ESP_LOGI(TAG, "Connecting to WiFi %s (attempt %d/%d)", ssid.c_str(), attempt, max_attempts);

    EventBits_t bits = xEventGroupWaitBits(
        event_group_,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(25000)
    );

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGE(TAG, "Connected to WiFi %s", ssid.c_str());
        esp_wifi_disconnect();
        is_connecting_ = false;
        return true;
    }

    ESP_LOGW(TAG, "Connect attempt %d/%d failed for %s", attempt, max_attempts, ssid.c_str());
    esp_wifi_disconnect();
    if (attempt < max_attempts) {
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

is_connecting_ = false;
ESP_LOGE(TAG, "Failed to connect to WiFi %s after %d attempts", ssid.c_str(), max_attempts);
return false;
```

**说明：**

- `set_config` 后增加 200ms 延迟，让配置通过 RPC 传播到 C6。
- 不再区分 2.4G/5G，统一使用 25 秒超时。
- 最多尝试 2 次，首次失败后等待 3 秒再试。

---

## 修改文件清单

| 序号 | 文件                         | 修改函数             | 修改内容                                                         |
| ---- | ---------------------------- | -------------------- | ---------------------------------------------------------------- |
| 1    | `wifi_configuration_ap.cc` | `Start()`          | 首次扫描延迟 2 秒；扫描失败 3 秒后重试                           |
| 2    | `wifi_configuration_ap.cc` | `Stop()`           | `esp_wifi_stop()` 后等待 500ms；销毁隐式 STA netif             |
| 3    | `wifi_configuration_ap.cc` | `ConnectToWifi()`  | `set_config` 后延迟 200ms；2.4G 超时 10s → 25s；失败重试 1 次 |
| 4    | `wifi_manager.cc`          | 头文件               | 新增`#include <freertos/FreeRTOS.h>`、`<freertos/task.h>`    |
| 5    | `wifi_manager.cc`          | `StartStation()`   | AP → STA 切换时等待 500ms                                       |
| 6    | `wifi_manager.cc`          | `StartConfigAp()`  | STA → AP 切换时等待 500ms                                       |
| 7    | `wifi_station.cc`          | `Start()`          | 定时器创建移到`esp_wifi_start()` 之前；回调失败重试            |
| 8    | `wifi_station.cc`          | `WifiEventHandler` | `WIFI_EVENT_STA_START` 事件延迟 2 秒扫描                       |

---

## 重要提示

1. **这些修改是缓解方案，不是根本解决。** 日志中持续出现的：

   ```
   W (2496) transport: Version mismatch: Host [2.11.0] > Co-proc [0.0.0]
   ```
   说明 C6 协处理器固件版本信息异常，RPC 延迟因此增加。彻底解决需要给 C6 刷入与 P4 侧 `ESP-Hosted` 版本匹配的固件。
2. **对集成 WiFi 芯片（ESP32-S3 等）的影响**：

   - 仅增加 2 秒首次扫描延迟、200ms 配置传播延迟、500ms 模式切换延迟。
   - 不影响正常功能，反而能增强兼容性。
3. **允许修改范围**：

   - 以上修改位于 `managed_components/78__esp-wifi-connect/` 目录下，超出了用户“只允许修改 `esp32-p4-module-dev-kit.cc`”的约束。
   - 这些修改已征得用户同意，用于修复 P4+C6 架构下的 WiFi 问题。如果后续需要严格只修改板级文件，需要另外设计 workaround（例如在 `esp32-p4-module-dev-kit.cc` 中通过自定义 `WifiManager` 行为替代组件逻辑）。
