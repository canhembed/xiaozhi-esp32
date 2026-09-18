#include "weather_manager.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>

static const char* TAG = "WeatherManager";

#define MAX_HTTP_RECV_BUFFER 2048

esp_err_t WeatherManager::Initialize() {
    LoadLocation();
    xTaskCreate(weather_task, "weather_task", 6144, this, 2, nullptr);
    return ESP_OK;
}

void WeatherManager::SetLocation(float latitude, float longitude) {
    latitude_ = latitude;
    longitude_ = longitude;
    SaveLocation();
    
    // Fetch immediately after location changes
    FetchWeather();
}

void WeatherManager::GetLocation(float& latitude, float& longitude) const {
    latitude = latitude_;
    longitude = longitude_;
}

WeatherData WeatherManager::GetCurrentWeather() const {
    return current_weather_;
}

void WeatherManager::LoadLocation() {
    Settings settings("weather", false);
    std::string lat_str = settings.GetString("latitude", "10.76");
    std::string lon_str = settings.GetString("longitude", "106.66");
    try {
        latitude_ = std::stof(lat_str);
        longitude_ = std::stof(lon_str);
    } catch (...) {
        latitude_ = 10.76f;
        longitude_ = 106.66f;
    }
}

void WeatherManager::SaveLocation() {
    Settings settings("weather", false);
    settings.SetString("latitude", std::to_string(latitude_));
    settings.SetString("longitude", std::to_string(longitude_));
}

void WeatherManager::weather_task(void* arg) {
    WeatherManager* manager = static_cast<WeatherManager*>(arg);
    
    // Wait for a while after boot before making the first request to allow WiFi to connect
    vTaskDelay(pdMS_TO_TICKS(15000));

    while (true) {
        manager->FetchWeather();
        // Fetch every 1 hour
        vTaskDelay(pdMS_TO_TICKS(3600000));
    }
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            {
                // Copy data to the buffer passed via user_data
                std::string* response = static_cast<std::string*>(evt->user_data);
                if (response) {
                    response->append(static_cast<char*>(evt->data), evt->data_len);
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

void WeatherManager::FetchWeather() {
    char url[256];
    snprintf(url, sizeof(url), 
        "https://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f&current=temperature_2m,relative_humidity_2m,weather_code", 
        latitude_, longitude_);

    ESP_LOGI(TAG, "Fetching weather from: %s", url);

    std::string response_buffer;

    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = http_event_handler;
    config.user_data = &response_buffer;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 10000;
    config.buffer_size = 2048; // Prevent ESP_ERR_INVALID_RESPONSE for large headers

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return;
    }
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200) {
            cJSON* root = cJSON_Parse(response_buffer.c_str());
            if (root) {
                cJSON* current_weather = cJSON_GetObjectItem(root, "current");
                if (current_weather) {
                    cJSON* temp = cJSON_GetObjectItem(current_weather, "temperature_2m");
                    cJSON* hum = cJSON_GetObjectItem(current_weather, "relative_humidity_2m");
                    cJSON* weathercode = cJSON_GetObjectItem(current_weather, "weather_code");
                    
                    if (temp && weathercode && hum) {
                        current_weather_.temperature = temp->valuedouble;
                        current_weather_.humidity = hum->valuedouble;
                        current_weather_.weather_code = weathercode->valueint;
                        current_weather_.is_valid = true;
                        current_weather_.last_update_time = pdTICKS_TO_MS(xTaskGetTickCount());
                        
                        ESP_LOGI(TAG, "Weather updated: %.1f°C, %.1f%%, Code: %d", 
                                current_weather_.temperature, current_weather_.humidity, current_weather_.weather_code);
                    }
                }
                cJSON_Delete(root);
            }
        } else {
            ESP_LOGE(TAG, "HTTP GET request failed with status: %d", status_code);
        }
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}
