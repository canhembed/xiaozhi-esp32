#ifndef RADIO_PLAYER_H
#define RADIO_PLAYER_H

#include <string>
#include <mutex>
#include <atomic>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class RadioPlayer {
public:
    RadioPlayer();
    ~RadioPlayer();

    // Start playing radio stream from URL
    bool Start(std::string audio_url);

    // Stop current stream
    void Stop();

    // Check if player is currently active
    bool IsPlaying() const { return active_; }

private:
    static void WorkerEntry(void* arg);
    void WorkerLoop();

    std::string current_url_;
    
    std::mutex mutex_;
    std::atomic<bool> active_{false};
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> worker_running_{false};
    TaskHandle_t task_handle_ = nullptr;
    uint32_t playback_id_ = 0;
};

#endif // RADIO_PLAYER_H
