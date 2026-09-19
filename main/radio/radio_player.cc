#include "radio_player.h"

#include <esp_log.h>
#include <vector>
#include "application.h"
#include "audio_service.h"
#include "board.h"
#include "http.h"

namespace {
constexpr int kHttpTimeoutMs = 60000;
constexpr size_t kHttpReadBufferSize = 4096;  // 4KB
constexpr uint32_t kRadioTaskStackSize = 12288;
constexpr UBaseType_t kRadioTaskPriority = 2;
const char* TAG = "RadioPlayer";

bool IsSupportedUrl(const std::string& url) {
    return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
}
}  // namespace

RadioPlayer::RadioPlayer() {}

RadioPlayer::~RadioPlayer() { Stop(); }

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

    std::string target_url = current_url_;
    // Always use HTTP to save RAM - SSL handshake consumes ~50KB which causes OOM during radio
    if (target_url.rfind("https://", 0) == 0) {
        target_url.replace(0, 8, "http://");
        ESP_LOGI(TAG, "Downgraded URL to HTTP: %s", target_url.c_str());
    }

    while (!cancelled_) {
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
        if (!http) {
            ESP_LOGE(TAG, "Failed to create HTTP client");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int redirect_count = 0;
        bool opened = false;
        std::string current_request_url = target_url;

        while (redirect_count < 5 && !cancelled_) {
            http->SetTimeout(kHttpTimeoutMs);
            http->SetHeader("Accept", "audio/aac, audio/aacp, audio/mpeg, audio/mp3");
            http->SetHeader("Accept-Encoding", "identity");

            ESP_LOGI(TAG, "Opening radio stream: %s", current_request_url.c_str());
            http->SetHeader("User-Agent",
                            "VLC/3.0.16 LibVLC/3.0.16");  // Zeno.fm requires a User-Agent
            if (http->Open("GET", current_request_url)) {
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
                    current_request_url = location;
                    // Downgrade HTTPS to HTTP on redirect
                    if (current_request_url.rfind("https://", 0) == 0) {
                        current_request_url.replace(0, 8, "http://");
                    }
                    redirect_count++;
                    http->Close();
                    ESP_LOGI(TAG, "Redirecting to: %s", current_request_url.c_str());
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
            // Detect audio format from Content-Type header
            uint8_t audio_format = 2;  // Default to AAC
            std::string content_type = http->GetResponseHeader("content-type");
            if (content_type.empty())
                content_type = http->GetResponseHeader("Content-Type");
            if (content_type.find("mpeg") != std::string::npos ||
                content_type.find("mp3") != std::string::npos) {
                audio_format = 1;  // MP3
                ESP_LOGI(TAG, "Stream format: MP3 (%s)", content_type.c_str());
            } else {
                ESP_LOGI(TAG, "Stream format: AAC (%s)", content_type.c_str());
            }

            std::vector<char> buffer(kHttpReadBufferSize);
            std::vector<uint8_t> accumulator;

            // Pre-buffering: MP3 streams are typically higher bitrate (128kbps+), so they need a larger 
            // buffer (64KB = 4 seconds) to prevent stuttering. AAC is typically lower bitrate (64kbps),
            // so 32KB is enough for a 4-second buffer, keeping startup fast for both.
            size_t current_chunk_size = (audio_format == kAudioFormatMp3) ? 65536 : 32768;
            accumulator.reserve(current_chunk_size);

            while (!cancelled_) {
                int size = http->Read(buffer.data(), buffer.size());
                if (size < 0) {
                    ESP_LOGE(TAG, "Radio HTTP read failed: %d", http->GetLastError());
                    break;  // Break to reconnect
                }
                if (size == 0) {
                    ESP_LOGW(TAG, "Radio HTTP stream ended or EOF");
                    break;  // Break to reconnect
                }

                if (cancelled_) {
                    break;
                }

                accumulator.insert(accumulator.end(), buffer.data(), buffer.data() + size);

                // Push when accumulator reaches the current dynamic chunk size
                if (accumulator.size() >= current_chunk_size) {
                    auto packet = std::make_unique<AudioStreamPacket>();
                    packet->format = audio_format;
                    packet->playback_id = current_playback_id;
                    packet->payload = std::move(accumulator);

                    Application::GetInstance().GetAudioService().PushPacketToDecodeQueue(
                        std::move(packet), true);

                    // After the first pre-buffer chunk, switch to 16KB chunks for frequent feeding
                    if (current_chunk_size > 16384) {
                        current_chunk_size = 16384;
                    }

                    accumulator.clear();
                    accumulator.reserve(current_chunk_size);
                }
            }
        }

        http->Close();
        http.reset();

        if (!cancelled_) {
            ESP_LOGW(TAG, "Radio stream disconnected, reconnecting in 3 seconds...");
            for (int i = 0; i < 30 && !cancelled_; i++) {
                vTaskDelay(pdMS_TO_TICKS(100));  // 3 seconds total
            }
        }
    }

    // Reset state
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;
        worker_running_ = false;
    }

    // Notify application that radio stopped (if it wasn't cancelled intentionally)
    if (!cancelled_) {
        Application::GetInstance().Schedule(
            []() { Application::GetInstance().SetDeviceState(kDeviceStateIdle); });
    }

    vTaskDelete(NULL);
}
