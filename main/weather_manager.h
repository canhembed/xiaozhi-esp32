#ifndef WEATHER_MANAGER_H
#define WEATHER_MANAGER_H

#include <string>
#include <esp_err.h>

struct WeatherData {
    float temperature;
    float humidity;
    int weather_code; // WMO Weather interpretation codes
    bool is_valid;
    uint32_t last_update_time;
};

class WeatherManager {
public:
    static WeatherManager& GetInstance() {
        static WeatherManager instance;
        return instance;
    }

    esp_err_t Initialize();
    
    void SetLocation(float latitude, float longitude);
    void GetLocation(float& latitude, float& longitude) const;
    
    WeatherData GetCurrentWeather() const;

private:
    WeatherManager() = default;
    
    void LoadLocation();
    void SaveLocation();
    
    static void weather_task(void* arg);
    void FetchWeather();

    float latitude_ = 10.76f;   // Default: HCMC
    float longitude_ = 106.66f;
    WeatherData current_weather_ = {0, 0, false, 0};
};

#endif // WEATHER_MANAGER_H
