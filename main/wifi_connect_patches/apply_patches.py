#!/usr/bin/env python3
"""
Apply WiFi provisioning patches for ESP32-P4 + C6 ESP-Hosted architecture.

The 78__esp-wifi-connect component was designed for integrated WiFi chips.
On P4+C6 ESP-Hosted, WiFi runs on the C6 co-processor via SDIO RPC, which
introduces extra latency. This patch adjusts timing and cleanup logic.

Usage:
    python apply_patches.py <project_dir>

The script is idempotent - it checks for a marker before applying each patch.
"""

import hashlib
import json
import os
import sys
import re


def compute_sha256(filepath):
    """Compute SHA256 hash of a file."""
    sha256 = hashlib.sha256()
    with open(filepath, 'rb') as f:
        for chunk in iter(lambda: f.read(65536), b''):
            sha256.update(chunk)
    return sha256.hexdigest()


def update_checksums(component_dir, patches):
    """
    Update CHECKSUMS.json and .component_hash after patches are applied.
    """
    checksums_path = os.path.join(component_dir, 'CHECKSUMS.json')
    component_hash_path = os.path.join(component_dir, '.component_hash')

    # Read existing checksums
    with open(checksums_path, 'r') as f:
        checksums = json.load(f)

    # Update checksums for modified files
    modified_files = set(p['file'] for p in patches)
    updated = False
    for entry in checksums['files']:
        if entry['path'] in modified_files:
            filepath = os.path.join(component_dir, entry['path'])
            if os.path.exists(filepath):
                new_hash = compute_sha256(filepath)
                new_size = os.path.getsize(filepath)
                if entry['hash'] != new_hash:
                    print(f"  Updated checksum: {entry['path']}")
                    entry['hash'] = new_hash
                    entry['size'] = new_size
                    updated = True

    if not updated:
        print("  All checksums already up to date.")
        return

    # Write updated CHECKSUMS.json
    with open(checksums_path, 'w') as f:
        json.dump(checksums, f, indent=2)
        f.write('\n')
    print(f"  Updated CHECKSUMS.json")

    # Update .component_hash (SHA256 of CHECKSUMS.json)
    new_component_hash = compute_sha256(checksums_path)
    with open(component_hash_path, 'w') as f:
        f.write(new_component_hash)
    print(f"  Updated .component_hash: {new_component_hash}")


def apply_patches(project_dir):
    """Apply all patches to the managed wifi component."""
    component_dir = os.path.join(project_dir, 'managed_components', '78__esp-wifi-connect')

    if not os.path.isdir(component_dir):
        print(f"ERROR: Component directory not found: {component_dir}")
        return False

    print(f"WiFi component directory: {component_dir}")

    # Define all patches: (file, marker, old_text, new_text)
    patches = [
        # Patch 1: wifi_configuration_ap.cc - Start() - delayed scan + retry
        {
            'file': 'wifi_configuration_ap.cc',
            'marker': 'On ESP32-P4 + C6 ESP-Hosted architecture, WiFi runs on C6 via SDIO RPC',
            'old': '''// Start scan immediately
    esp_wifi_scan_start(nullptr, false);
    // Setup periodic WiFi scan timer.
    // skip_unhandled_events = false so the timer can wake the CPU from light
    // sleep on its own; otherwise the AP-mode scan list would stop refreshing
    // whenever the user paused interacting with the config web UI. See
    // esp_timer_get_next_alarm_for_wake_up in components/esp_timer/src/esp_timer.c.
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
        .skip_unhandled_events = false
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &scan_timer_));''',
            'new': '''// Setup periodic WiFi scan timer.
    // skip_unhandled_events = false so the timer can wake the CPU from light
    // sleep on its own; otherwise the AP-mode scan list would stop refreshing
    // whenever the user paused interacting with the config web UI. See
    // esp_timer_get_next_alarm_for_wake_up in components/esp_timer/src/esp_timer.c.
    //
    // On ESP32-P4 + C6 ESP-Hosted architecture, WiFi runs on C6 via SDIO RPC
    // and may not be immediately ready after esp_wifi_start(). Delay the first
    // scan by 2s to allow the co-processor to settle. The callback also retries
    // on failure (3s interval).
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
        .skip_unhandled_events = false
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &scan_timer_));

    // Delay first scan by 2s to allow co-processor (ESP32-C6) to be ready.
    // On ESP32-P4 + C6 ESP-Hosted architecture, WiFi runs on C6 via SDIO RPC
    // and may not be immediately ready after esp_wifi_start().
    esp_timer_start_once(scan_timer_, 2 * 1000000);'''
        },
        # Patch 2: wifi_configuration_ap.cc - Stop() - delay + destroy implicit STA netif
        {
            'file': 'wifi_configuration_ap.cc',
            'marker': 'On ESP32-P4 + C6 ESP-Hosted, esp_wifi_stop() is an async RPC call',
            'old': '''// 停止WiFi（但不 deinit，WiFi 驱动由 WifiManager 管理）
    esp_wifi_stop();
    
    // 销毁网络接口
    if (ap_netif_) {
        esp_netif_destroy_default_wifi(ap_netif_);
        ap_netif_ = nullptr;
    }

    ESP_LOGI(TAG, "Wifi configuration AP stopped");''',
            'new': '''// 停止WiFi（但不 deinit，WiFi 驱动由 WifiManager 管理）
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

    ESP_LOGI(TAG, "Wifi configuration AP stopped");'''
        },
        # Patch 3: wifi_configuration_ap.cc - ConnectToWifi() - delay + unified timeout
        {
            'file': 'wifi_configuration_ap.cc',
            'marker': 'On P4+C6 ESP-Hosted with co-processor version mismatch, RPC',
            'old': '''ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        auto ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG,
                "esp_wifi_connect failed: %d (attempt %d/%d)",
                ret, attempt, kMaxAttempts);
            continue;
        }
        ESP_LOGI(TAG, "Connecting to WiFi %s (attempt %d/%d)",
            ssid.c_str(), attempt, kMaxAttempts);

        // Wait for the connection to complete for 10 or 25 seconds.
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
        );''',
            'new': '''ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        // On ESP32-P4 + C6 ESP-Hosted, allow time for RPC config to propagate to C6
        vTaskDelay(pdMS_TO_TICKS(200));
        auto ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG,
                "esp_wifi_connect failed: %d (attempt %d/%d)",
                ret, attempt, kMaxAttempts);
            continue;
        }
        ESP_LOGI(TAG, "Connecting to WiFi %s (attempt %d/%d)",
            ssid.c_str(), attempt, kMaxAttempts);

        // Wait for the connection to complete (25s for both 2.4G and 5G).
        // On P4+C6 ESP-Hosted with co-processor version mismatch, RPC
        // round-trips are slow. Use 25s for all bands to accommodate the
        // extra latency.
        EventBits_t bits = xEventGroupWaitBits(
            event_group_,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(25000)
        );'''
        },
        # Patch 4: wifi_station.cc - Start() - timer before esp_wifi_start + retry
        {
            'file': 'wifi_station.cc',
            'marker': 'Create scan timer BEFORE esp_wifi_start() so timer_handle_ is valid',
            'old': '''ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (max_tx_power_ != 0) {
        ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(max_tx_power_));
    }

    // Setup the timer to scan WiFi.
    // skip_unhandled_events = false so the timer can wake the CPU from light
    // sleep on its own; otherwise an idle device that failed to connect would
    // never retry the scan, because esp_timer_get_next_alarm_for_wake_up
    // (components/esp_timer/src/esp_timer.c) excludes timers with this flag
    // from light-sleep wakeup sources.
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            esp_wifi_scan_start(nullptr, false);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "WiFiScanTimer",
        .skip_unhandled_events = false
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
}''',
            'new': '''ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Create scan timer BEFORE esp_wifi_start() so timer_handle_ is valid
    // when WIFI_EVENT_STA_START fires. On ESP32-P4 + C6 ESP-Hosted, the
    // STA_START event may fire before this function returns.
    // skip_unhandled_events = false so the timer can wake the CPU from light
    // sleep on its own; otherwise an idle device that failed to connect would
    // never retry the scan, because esp_timer_get_next_alarm_for_wake_up
    // (components/esp_timer/src/esp_timer.c) excludes timers with this flag
    // from light-sleep wakeup sources.
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
        .skip_unhandled_events = false
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));

    ESP_ERROR_CHECK(esp_wifi_start());

    if (max_tx_power_ != 0) {
        ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(max_tx_power_));
    }
}'''
        },
        # Patch 5: wifi_station.cc - WifiEventHandler - STA_START delayed scan
        {
            'file': 'wifi_station.cc',
            'marker': 'Delay scan by 2s for ESP32-P4 + C6 ESP-Hosted to allow co-processor',
            'old': '''if (event_id == WIFI_EVENT_STA_START) {
        esp_wifi_scan_start(nullptr, false);
        if (this_->on_scan_begin_) {
            this_->on_scan_begin_();
        }
    }''',
            'new': '''if (event_id == WIFI_EVENT_STA_START) {
        // Delay scan by 2s for ESP32-P4 + C6 ESP-Hosted to allow co-processor
        // to be ready. On integrated WiFi chips this adds minimal latency.
        // The timer callback also retries on failure (3s interval).
        esp_timer_start_once(this_->timer_handle_, 2 * 1000000);
        if (this_->on_scan_begin_) {
            this_->on_scan_begin_();
        }
    }'''
        },
        # Patch 6: wifi_manager.cc - add freertos headers
        {
            'file': 'wifi_manager.cc',
            'marker': '#include <freertos/FreeRTOS.h>',
            'old': '''#include "wifi_manager.h"
#include "wifi_station.h"
#include "wifi_configuration_ap.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_mac.h>
#include <nvs_flash.h>''',
            'new': '''#include "wifi_manager.h"
#include "wifi_station.h"
#include "wifi_configuration_ap.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_mac.h>
#include <nvs_flash.h>'''
        },
        # Patch 7: wifi_manager.cc - StartStation() - 500ms delay
        {
            'file': 'wifi_manager.cc',
            'marker': 'Delay for co-processor to fully release WiFi resources after APSTA stop',
            'old': '''// Auto-stop config AP if active
    if (config_mode_active_) {
        ESP_LOGI(TAG, "Stopping config AP before starting station");
        config_ap_->Stop();
        config_mode_active_ = false;
        // Notify outside lock
        lock.unlock();
        NotifyEvent(WifiEvent::ConfigModeExit);
        lock.lock();
    }''',
            'new': '''// Auto-stop config AP if active
    if (config_mode_active_) {
        ESP_LOGI(TAG, "Stopping config AP before starting station");
        config_ap_->Stop();
        config_mode_active_ = false;
        // Notify outside lock
        lock.unlock();
        NotifyEvent(WifiEvent::ConfigModeExit);
        // Delay for co-processor to fully release WiFi resources after APSTA stop.
        // On P4+C6 ESP-Hosted, esp_wifi_stop() is async and netif cleanup may lag.
        vTaskDelay(pdMS_TO_TICKS(500));
        lock.lock();
    }'''
        },
        # Patch 8: wifi_manager.cc - StartConfigAp() - 500ms delay
        {
            'file': 'wifi_manager.cc',
            'marker': 'Delay for co-processor cleanup before starting AP mode',
            'old': '''// Auto-stop station if active
    if (station_active_) {
        ESP_LOGI(TAG, "Stopping station before starting config AP");
        station_->Stop();
        station_active_ = false;
        lock.unlock();
        NotifyEvent(WifiEvent::Disconnected);
        lock.lock();
    }''',
            'new': '''// Auto-stop station if active
    if (station_active_) {
        ESP_LOGI(TAG, "Stopping station before starting config AP");
        station_->Stop();
        station_active_ = false;
        lock.unlock();
        NotifyEvent(WifiEvent::Disconnected);
        // Delay for co-processor cleanup before starting AP mode
        vTaskDelay(pdMS_TO_TICKS(500));
        lock.lock();
    }'''
        },
    ]

    # Check and apply each patch
    patches_applied = []
    for i, patch in enumerate(patches):
        filepath = os.path.join(component_dir, patch['file'])
        if not os.path.exists(filepath):
            print(f"  [{i+1}] SKIP: {patch['file']} not found")
            continue

        with open(filepath, 'r', encoding='utf-8') as f:
            content = f.read()

        # Check if patch is already applied by looking for the marker
        if patch['marker'] in content:
            print(f"  [{i+1}] OK:   {patch['file']} - already patched")
            continue

        # Check if the old text exists
        if patch['old'] not in content:
            print(f"  [{i+1}] WARN: {patch['file']} - old text not found, skipping")
            continue

        # Apply the patch
        new_content = content.replace(patch['old'], patch['new'], 1)
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(new_content)

        print(f"  [{i+1}] APPLIED: {patch['file']}")
        patches_applied.append(patch['file'])

    if not patches_applied:
        print("\nAll patches already applied. Verifying checksums...")
        verify_checksums(component_dir, patches)
        return True

    print(f"\nApplied {len(patches_applied)} patches. Updating checksums...")
    update_checksums(component_dir, patches)
    return True


def verify_checksums(component_dir, patches):
    """Verify and update checksums without applying patches."""
    checksums_path = os.path.join(component_dir, 'CHECKSUMS.json')
    component_hash_path = os.path.join(component_dir, '.component_hash')

    with open(checksums_path, 'r') as f:
        checksums = json.load(f)

    modified_files = set(p['file'] for p in patches)
    needs_update = False
    for entry in checksums['files']:
        if entry['path'] in modified_files:
            filepath = os.path.join(component_dir, entry['path'])
            if os.path.exists(filepath):
                actual_hash = compute_sha256(filepath)
                actual_size = os.path.getsize(filepath)
                if entry['hash'] != actual_hash or entry['size'] != actual_size:
                    print(f"  Checksum mismatch: {entry['path']}")
                    print(f"    Expected: {entry['hash']} ({entry['size']} bytes)")
                    print(f"    Actual:   {actual_hash} ({actual_size} bytes)")
                    entry['hash'] = actual_hash
                    entry['size'] = actual_size
                    needs_update = True

    if not needs_update:
        print("  All checksums match. No update needed.")
        return

    with open(checksums_path, 'w') as f:
        json.dump(checksums, f, indent=2)
        f.write('\n')
    print(f"  Updated CHECKSUMS.json")

    new_component_hash = compute_sha256(checksums_path)
    with open(component_hash_path, 'w') as f:
        f.write(new_component_hash)
    print(f"  Updated .component_hash: {new_component_hash}")


def main():
    if len(sys.argv) < 2:
        print("Usage: python apply_patches.py <project_dir>")
        sys.exit(1)

    project_dir = os.path.abspath(sys.argv[1])
    print(f"Project directory: {project_dir}")
    print("Checking WiFi patches for ESP32-P4 + C6 ESP-Hosted...")
    print()

    if not apply_patches(project_dir):
        sys.exit(1)

    print("\nDone.")


if __name__ == '__main__':
    main()