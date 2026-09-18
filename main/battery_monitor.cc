#include "battery_monitor.h"
#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>
#include "application.h"
#include "boards/common/board.h"
#include "display.h"

#define TAG "BatteryMonitor"

static void battery_monitor_task(void* arg) {
    auto& board = Board::GetInstance();
    int battery_level;
    bool charging;
    bool discharging;

    uint32_t last_alert_tick = 0;
    const uint32_t ALERT_COOLDOWN_MS = 600000;  // 10 minutes

    uint32_t last_active_tick = pdTICKS_TO_MS(xTaskGetTickCount());
    // Initial random timeout between 2 and 5 hours (in milliseconds)
    uint32_t current_idle_timeout_ms = 3600000 * (2 + (esp_random() % 4));

    while (true) {
        if (board.GetBatteryLevel(battery_level, charging, discharging)) {
            // Check if battery is low (<= 15%) and NOT charging
            if (battery_level <= 15 && !charging) {
                uint32_t current_tick = pdTICKS_TO_MS(xTaskGetTickCount());

                if (last_alert_tick == 0 || (current_tick - last_alert_tick >= ALERT_COOLDOWN_MS)) {
                    ESP_LOGI(TAG, "Battery low (%d%%), triggering alert", battery_level);

                    // Only trigger alert if we are in idle or listening state
                    auto state = Application::GetInstance().GetDeviceState();
                    if (state == kDeviceStateIdle || state == kDeviceStateListening) {
                        Application::GetInstance().Chat("battery low");
                        last_alert_tick = current_tick;
                    }
                }
            } else if (charging) {
                // If it's charging, we can reset the alert cooldown
                last_alert_tick = 0;
            }
        }

        uint32_t current_tick = pdTICKS_TO_MS(xTaskGetTickCount());
        auto current_state = Application::GetInstance().GetDeviceState();

        if (current_state != kDeviceStateIdle) {
            last_active_tick = current_tick;
            Board::GetInstance().GetDisplay()->ShowScreensaver(false);
        } else {
            uint32_t idle_time = current_tick - last_active_tick;

            // Show screensaver after 1 minute of idle
            if (idle_time >= 60000) {
                Board::GetInstance().GetDisplay()->ShowScreensaver(true);
            }

            if (idle_time >= current_idle_timeout_ms) {
                time_t now = time(nullptr);
                struct tm timeinfo;
                localtime_r(&now, &timeinfo);

                // Do Not Disturb mode: Disable idle alerts between 22:00 and 07:00
                if (timeinfo.tm_year >= (2025 - 1900) &&
                    (timeinfo.tm_hour >= 22 || timeinfo.tm_hour < 7)) {
                    ESP_LOGI(TAG,
                             "Device idle for %lu ms, but it's nighttime (DND), skipping alert",
                             current_idle_timeout_ms);
                } else {
                    ESP_LOGI(TAG, "Device idle for %lu ms, triggering idle alert",
                             current_idle_timeout_ms);
                    Application::GetInstance().Chat("idle_alert");
                    Board::GetInstance().GetDisplay()->ShowScreensaver(false);
                }

                // Always reset the timer and pick a NEW random timeout for the next cycle
                last_active_tick = current_tick;
                current_idle_timeout_ms = 3600000 * (2 + (esp_random() % 4));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(60000));  // Check every 60 seconds
    }
}

esp_err_t BatteryMonitor::Initialize() {
    xTaskCreate(battery_monitor_task, "battery_monitor", 4096, nullptr, 2, nullptr);
    ESP_LOGI(TAG, "Battery monitor initialized");
    return ESP_OK;
}
