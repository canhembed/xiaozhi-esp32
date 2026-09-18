#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

#include <esp_err.h>

class BatteryMonitor {
public:
    static esp_err_t Initialize();
};

#endif // BATTERY_MONITOR_H
