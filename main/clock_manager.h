#ifndef CLOCK_MANAGER_H
#define CLOCK_MANAGER_H

#include <string>
#include <vector>
#include <esp_err.h>
#include <time.h>

struct Alarm {
    int id;
    int hour;
    int minute;
    uint8_t days_of_week; // Bitmask: 0x01=Sun, 0x02=Mon ... 0x40=Sat. 0x7F=Everyday
    int year;             // -1 if unused
    int month;            // -1 if unused
    int day;              // -1 if unused
    bool enabled;
};

class ClockManager {
public:
    static ClockManager& GetInstance() {
        static ClockManager instance;
        return instance;
    }

    esp_err_t Initialize();
    
    // SNTP Setup
    void SyncTime();
    
    // Time access
    void GetCurrentTime(int& hour, int& minute, int& second);
    std::string GetCurrentTimeStr();

    // Alarm management
    void AddAlarm(int hour, int minute, uint8_t days_of_week = 0x7F, int year = -1, int month = -1, int day = -1);
    void RemoveAlarm(int id);
    void SetAlarmEnabled(int id, bool enabled);
    std::vector<Alarm> GetAlarms() const;

    int GetAlarmSoundId() const { return alarm_sound_id_; }
    void SetAlarmSoundId(int id);

    int GetAlarmVolume() const { return alarm_volume_; }
    void SetAlarmVolume(int volume);

private:
    ClockManager() = default;
    void LoadAlarms();
    void SaveAlarms();
    void CheckAlarms();

    static void alarm_task(void* arg);

    std::vector<Alarm> alarms_;
    int next_alarm_id_ = 1;
    int alarm_sound_id_ = 1;
    int alarm_volume_ = 100;
};

#endif // CLOCK_MANAGER_H
