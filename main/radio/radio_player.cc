#include "radio_player.h"

#include <esp_log.h>
#include "board.h"
#include "http.h"
#include "audio_service.h"
#include "application.h"

namespace {
constexpr int kHttpTimeoutMs = 5000;
constexpr size_t kHttpReadBufferSize = 4096;
constexpr uint32_t kRadioTaskStackSize = 8192;
constexpr UBaseType_t kRadioTaskPriority = 2;
const char* TAG = "RadioPlayer";

bool IsSupportedUrl(const std::string& url) {
    return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
}
}  // namespace

RadioPlayer::RadioPlayer() {
}

RadioPlayer::~RadioPlayer() {
    Stop();
}

bool RadioPlayer::Start(std::string audio_url) {
    if (!IsSupportedUrl(audio_url)) {
        ESP_LOGE(TAG, "Unsupported URL format: %s", audio_url.c_str());
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (active_) {
        ESP_LOGW(TAG, "Radio is already playing, stop it first");
        return false;
    }

    current_url_ = std::move(audio_url);
    active_ = true;
    cancelled_ = false;
    worker_running_ = true;
    playback_id_++;

    BaseType_t created = xTaskCreate(WorkerEntry, "radio_http", kRadioTaskStackSize, this,
                                     kRadioTaskPriority, &task_handle_);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create radio HTTP task");
        active_ = false;
        worker_running_ = false;
        return false;
    }

    return true;
}

void RadioPlayer::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || cancelled_) {
            return;
        }
        cancelled_ = true;
    }

    // Wait for the worker to exit
    while (worker_running_) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void RadioPlayer::WorkerEntry(void* arg) {
    auto* player = static_cast<RadioPlayer*>(arg);
    player->WorkerLoop();
}

void RadioPlayer::WorkerLoop() {
    uint32_t current_playback_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_playback_id = playback_id_;
    }

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return;
    }

    std::string target_url = current_url_;
    int redirect_count = 0;
    bool opened = false;

    while (redirect_count < 5 && !cancelled_) {
        http->SetTimeout(kHttpTimeoutMs);
        http->SetHeader("Accept", "audio/mpeg, audio/mp3");
        http->SetHeader("Accept-Encoding", "identity");

        ESP_LOGI(TAG, "Opening radio stream: %s", target_url.c_str());
        if (http->Open("GET", target_url)) {
            int status = http->GetStatusCode();
            if (status >= 200 && status < 300) {
                opened = true;
                break;
            } else if (status == 301 || status == 302 || status == 307 || status == 308) {
                std::string location = http->GetResponseHeader("Location");
                if (location.empty()) {
                    ESP_LOGE(TAG, "Redirect without Location header");
                    break;
                }
                target_url = location;
                redirect_count++;
                http->Close();
                ESP_LOGI(TAG, "Redirecting to: %s", target_url.c_str());
            } else {
                ESP_LOGE(TAG, "HTTP error: %d", status);
                break;
            }
        } else {
            ESP_LOGE(TAG, "Failed to open HTTP connection");
            break;
        }
    }

    if (opened && !cancelled_) {
        std::array<char, kHttpReadBufferSize> buffer;
        while (!cancelled_) {
            int size = http->Read(buffer.data(), buffer.size());
            if (size < 0) {
                ESP_LOGE(TAG, "Radio HTTP read failed: %d", http->GetLastError());
                break;
            }
            if (size == 0) {
                ESP_LOGW(TAG, "Radio HTTP stream ended or EOF");
                break;
            }
            
            if (cancelled_) {
                break;
            }

        // Push directly to AudioService
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->format = 1; // MP3
        packet->playback_id = current_playback_id;
        packet->payload.assign(buffer.data(), buffer.data() + size);
        
        Application::GetInstance().GetAudioService().PushPacketToDecodeQueue(std::move(packet), true);
        }
    }
    http->Close();
    http.reset();

    // Reset state
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;
        worker_running_ = false;
    }

    // Notify application that radio stopped (if it wasn't cancelled intentionally)
    if (!cancelled_) {
        Application::GetInstance().Schedule([]() {
            Application::GetInstance().SetDeviceState(kDeviceStateIdle);
        });
    }

    vTaskDelete(NULL);
}
