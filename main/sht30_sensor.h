#ifndef _SHT30_SENSOR_H_
#define _SHT30_SENSOR_H_

#include <esp_err.h>

class Sht30Sensor {
public:
    static esp_err_t Initialize();
    static esp_err_t Read(float& temperature, float& humidity);
    static void SetAlertEnabled(bool enabled);
};

#endif // _SHT30_SENSOR_H_
