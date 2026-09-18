#include "clock_manager.h"
#include "application.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <freertos/FreeRTOS.h>
#include "assets/lang_config.h"
#include <freertos/task.h>
#include <cJSON.h>

static const char* TAG = "ClockManager";

esp_err_t ClockManager::Initialize() {
    LoadAlarms();

    Settings settings("alarms", false);
    alarm_sound_id_ = settings.GetInt("sound_id", 1);
    alarm_volume_ = settings.GetInt("alarm_volume", 100);

    // Configure SNTP
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);
    
    // Set Timezone to Vietnam (UTC+7)
    setenv("TZ", "VST-7", 1);
    tzset();

    xTaskCreate(alarm_task, "alarm_task", 4096, this, 2, nullptr);
    return ESP_OK;
}

void ClockManager::SyncTime() {
    // esp_netif_sntp_init automatically handles syncing in the background
    // We just need to make sure we wait for it if necessary, but for a clock,
    // it will eventually sync.
    ESP_LOGI(TAG, "Time sync initiated...");
}

void ClockManager::GetCurrentTime(int& hour, int& minute, int& second) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    hour = timeinfo.tm_hour;
    minute = timeinfo.tm_min;
    second = timeinfo.tm_sec;
}

std::string ClockManager::GetCurrentTimeStr() {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    char strftime_buf[64];
    // Check if year is >= 2024 (124) to ensure time is set
    if (timeinfo.tm_year < (2024 - 1900)) {
        return "--:--";
    }
    
    strftime(strftime_buf, sizeof(strftime_buf), "%H:%M", &timeinfo);
    return std::string(strftime_buf);
}

void ClockManager::AddAlarm(int hour, int minute, uint8_t days_of_week, int year, int month, int day) {
    Alarm alarm = {next_alarm_id_++, hour, minute, days_of_week, year, month, day, true};
    alarms_.push_back(alarm);
    SaveAlarms();
    ESP_LOGI(TAG, "Added alarm %d at %02d:%02d", alarm.id, hour, minute);
}

void ClockManager::RemoveAlarm(int id) {
    for (auto it = alarms_.begin(); it != alarms_.end(); ++it) {
        if (it->id == id) {
            alarms_.erase(it);
            SaveAlarms();
            ESP_LOGI(TAG, "Removed alarm %d", id);
            return;
        }
    }
}

void ClockManager::SetAlarmEnabled(int id, bool enabled) {
    for (auto& alarm : alarms_) {
        if (alarm.id == id) {
            alarm.enabled = enabled;
            SaveAlarms();
            ESP_LOGI(TAG, "Set alarm %d to %s", id, enabled ? "enabled" : "disabled");
            return;
        }
    }
}

std::vector<Alarm> ClockManager::GetAlarms() const {
    return alarms_;
}

void ClockManager::SetAlarmSoundId(int id) {
    if (id < 1) id = 1;
    // We don't strictly cap it at 2 here anymore, because we might have up to N sounds.
    // The cap will be handled gracefully by the array bounds check.
    alarm_sound_id_ = id;
    Settings settings("alarms", true);
    settings.SetInt("sound_id", alarm_sound_id_);
    ESP_LOGI(TAG, "Set alarm sound id to %d", alarm_sound_id_);
}

void ClockManager::SetAlarmVolume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    alarm_volume_ = volume;
    Settings settings("alarms", true);
    settings.SetInt("alarm_volume", alarm_volume_);
    ESP_LOGI(TAG, "Set alarm volume to %d%%", alarm_volume_);
}

void ClockManager::LoadAlarms() {
    Settings settings("alarms", false);
    std::string json_str = settings.GetString("list", "[]");
    
    cJSON* root = cJSON_Parse(json_str.c_str());
    if (root == nullptr) return;

    alarms_.clear();
    int max_id = 0;

    int num_alarms = cJSON_GetArraySize(root);
    for (int i = 0; i < num_alarms; i++) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        if (item == nullptr) continue;

        Alarm alarm;
        alarm.id = cJSON_GetObjectItem(item, "id")->valueint;
        alarm.hour = cJSON_GetObjectItem(item, "hour")->valueint;
        alarm.minute = cJSON_GetObjectItem(item, "minute")->valueint;
        
        cJSON* days_item = cJSON_GetObjectItem(item, "days_of_week");
        alarm.days_of_week = days_item ? days_item->valueint : 0x7F;
        
        cJSON* year_item = cJSON_GetObjectItem(item, "year");
        alarm.year = year_item ? year_item->valueint : -1;
        
        cJSON* month_item = cJSON_GetObjectItem(item, "month");
        alarm.month = month_item ? month_item->valueint : -1;
        
        cJSON* day_item = cJSON_GetObjectItem(item, "day");
        alarm.day = day_item ? day_item->valueint : -1;

        alarm.enabled = cJSON_GetObjectItem(item, "enabled")->valueint != 0;

        alarms_.push_back(alarm);
        if (alarm.id > max_id) {
            max_id = alarm.id;
        }
    }
    next_alarm_id_ = max_id + 1;
    cJSON_Delete(root);
}

void ClockManager::SaveAlarms() {
    cJSON* root = cJSON_CreateArray();
    for (const auto& alarm : alarms_) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", alarm.id);
        cJSON_AddNumberToObject(item, "hour", alarm.hour);
        cJSON_AddNumberToObject(item, "minute", alarm.minute);
        cJSON_AddNumberToObject(item, "days_of_week", alarm.days_of_week);
        cJSON_AddNumberToObject(item, "year", alarm.year);
        cJSON_AddNumberToObject(item, "month", alarm.month);
        cJSON_AddNumberToObject(item, "day", alarm.day);
        cJSON_AddBoolToObject(item, "enabled", alarm.enabled);
        cJSON_AddItemToArray(root, item);
    }
    
    char* json_str = cJSON_PrintUnformatted(root);
    Settings settings("alarms", true); // true for read_write
    settings.SetString("list", json_str);
    
    free(json_str);
    cJSON_Delete(root);
}

void ClockManager::alarm_task(void* arg) {
    ClockManager* manager = static_cast<ClockManager*>(arg);
    int last_triggered_minute = -1;

    while (true) {
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);
        
        // Only check if time is actually set
        if (timeinfo.tm_year >= (2024 - 1900)) {
            int current_hour = timeinfo.tm_hour;
            int current_minute = timeinfo.tm_min;

            if (current_minute != last_triggered_minute) {
                bool triggered = false;
                for (auto& alarm : manager->alarms_) {
                    if (alarm.enabled && alarm.hour == current_hour && alarm.minute == current_minute) {
                        bool should_ring = false;
                        bool is_one_time = false;
                        
                        // Check if it's a specific date alarm
                        if (alarm.year != -1 && alarm.month != -1 && alarm.day != -1) {
                            if (alarm.year == (timeinfo.tm_year + 1900) &&
                                alarm.month == (timeinfo.tm_mon + 1) &&
                                alarm.day == timeinfo.tm_mday) {
                                should_ring = true;
                                is_one_time = true;
                            }
                        } else {
                            // Check if it matches the current day of the week
                            // timeinfo.tm_wday is 0 for Sunday, 1 for Monday, etc.
                            if (alarm.days_of_week & (1 << timeinfo.tm_wday)) {
                                should_ring = true;
                                // If days_of_week is exactly today and not everyday, it might be a 1-time weekday alarm?
                                // Usually if it has a days_of_week bitmask, it's repeating.
                                // However, if the user requested a 1-time alarm and didn't specify date,
                                // the AI tool should set the date explicitly.
                                // Let's rely on date for 1-time, and days_of_week for repeating.
                                is_one_time = false; 
                            }
                        }

                        if (should_ring) {
                            ESP_LOGI(TAG, "ALARM %d TRIGGERED!", alarm.id);
                            triggered = true;
                            
                            if (is_one_time) {
                                alarm.enabled = false;
                            }
                        }
                    }
                }
                
                if (triggered) {
                    manager->SaveAlarms();
                    last_triggered_minute = current_minute;
                    
                    // Transition to AlarmRinging state
                    Application::GetInstance().SetDeviceState(kDeviceStateAlarmRinging);
                }
            }
        }
        
        // Handle alarm looping
        if (Application::GetInstance().GetDeviceState() == kDeviceStateAlarmRinging) {
            if (Application::GetInstance().IsPlaybackIdle()) {
                int sound_id = manager->GetAlarmSoundId();
                
                if (sound_id == 2) {
                    Application::GetInstance().PlaySound(Lang::Sounds::OGG_ALARM2);
                } 
                // else if (sound_id == 3) {
                //     Application::GetInstance().PlaySound(Lang::Sounds::OGG_ALARM3);
                // } 
                else {
                    Application::GetInstance().PlaySound(Lang::Sounds::OGG_ALARM1); // Mặc định
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
