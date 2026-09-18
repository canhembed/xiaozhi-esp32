/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_pthread.h>
#include <algorithm>
#include <cstring>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "clock_manager.h"
#include "display.h"
#include "lvgl_display.h"
#include "lvgl_theme.h"
#include "oled_display.h"
#include "settings.h"
#include "sht30_sensor.h"
#include "weather_manager.h"

#define TAG "MCP"

McpServer::McpServer() {}

McpServer::~McpServer() {
    for (auto tool : tools_) {
        delete tool;
    }
    tools_.clear();
}

void McpServer::AddCommonTools() {
    // *Important* To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // **重要** 为了提升响应速度，我们把常用的工具放在前面，利用 prompt cache 的特性。

    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    // Do not add custom tools here.
    // Custom tools must be added in the board's InitializeTools function.

    AddTool("self.get_device_status",
            "Provides the real-time information of the device, including the current status of the "
            "audio speaker, screen, battery, network, etc.\n"
            "Use this tool for: \n"
            "1. Answering questions about current condition (e.g. what is the current volume of "
            "the audio speaker?)\n"
            "2. As the first step to control the device (e.g. turn up / down the volume of the "
            "audio speaker, etc.)",
            PropertyList(), [&board](const PropertyList& properties) -> ReturnValue {
                return board.GetDeviceStatusJson();
            });

    AddTool("self.audio_speaker.set_volume",
            "Set the volume of the audio speaker. If the current volume is unknown, you must call "
            "`self.get_device_status` tool first and then call this tool.",
            PropertyList({Property("volume", kPropertyTypeInteger, 0, 100)}),
            [&board](const PropertyList& properties) -> ReturnValue {
                auto codec = board.GetAudioCodec();
                codec->SetOutputVolume(properties["volume"].value<int>());
                return true;
            });

    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness", "Set the brightness of the screen.",
                PropertyList({Property("brightness", kPropertyTypeInteger, 0, 100)}),
                [backlight](const PropertyList& properties) -> ReturnValue {
                    uint8_t brightness =
                        static_cast<uint8_t>(properties["brightness"].value<int>());
                    backlight->SetBrightness(brightness, true);
                    return true;
                });
    }

#ifdef HAVE_LVGL
    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        AddTool("self.screen.set_theme",
                "Set the theme of the screen. The theme can be `light` or `dark`.",
                PropertyList({Property("theme", kPropertyTypeString)}),
                [display](const PropertyList& properties) -> ReturnValue {
                    auto theme_name = properties["theme"].value<std::string>();
                    auto& theme_manager = LvglThemeManager::GetInstance();
                    auto theme = theme_manager.GetTheme(theme_name);
                    if (theme != nullptr) {
                        display->SetTheme(theme);
                        return true;
                    }
                    return false;
                });
    }

    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera.take_photo",
                "Always remember you have a camera. If the user asks you to see something, use "
                "this tool to take a photo and then explain it.\n"
                "Args:\n"
                "  `question`: The question that you want to ask about the photo.\n"
                "Return:\n"
                "  A JSON object that provides the photo information.",
                PropertyList({Property("question", kPropertyTypeString)}),
                [camera](const PropertyList& properties) -> ReturnValue {
                    // Lower the priority to do the camera capture
                    TaskPriorityReset priority_reset(1);

                    if (!camera->Capture()) {
                        throw std::runtime_error("Failed to capture photo");
                    }
                    auto question = properties["question"].value<std::string>();
                    return camera->Explain(question);
                });
    }
#endif

    // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
}

void McpServer::AddUserOnlyTools() {
    // System tools
    AddUserOnlyTool("self.get_system_info", "Get the system information", PropertyList(),
                    [this](const PropertyList& properties) -> ReturnValue {
                        auto& board = Board::GetInstance();
                        return board.GetSystemInfoJson();
                    });

    AddTool("self.get_environment_data",
            "Get the current room temperature and humidity. NOTE: If you receive a short message "
            "like 'hot 31.5C', it is an automated alert from the device indicating that the ROOM "
            "temperature is too hot (e.g. 31.5C). Please advise the user to turn on the fan or AC "
            "without asking if they are indoors or outdoors.",
            PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                Sht30Sensor::Initialize();
                float temp = 0;
                float hum = 0;
                if (Sht30Sensor::Read(temp, hum) == ESP_OK) {
                    cJSON* root = cJSON_CreateObject();
                    cJSON_AddNumberToObject(root, "temperature_celsius", temp);
                    cJSON_AddNumberToObject(root, "humidity_percent", hum);
                    return root;
                }
                return "Failed to read sensor";
            });

    AddUserOnlyTool("self.get_battery_status",
                    "Get the current battery level and charging status. NOTE: If you receive a "
                    "short message like 'battery low', it is an automated alert from the device "
                    "indicating the battery is below 15%. You should complain that you are hungry "
                    "and ask the user to plug in the charger.",
                    PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                        auto& board = Board::GetInstance();
                        int battery_level;
                        bool charging;
                        bool discharging;

                        if (board.GetBatteryLevel(battery_level, charging, discharging)) {
                            cJSON* root = cJSON_CreateObject();
                            cJSON_AddNumberToObject(root, "battery_level_percent", battery_level);
                            cJSON_AddBoolToObject(root, "is_charging", charging);
                            cJSON_AddBoolToObject(root, "is_discharging", discharging);
                            return root;
                        }
                        return "Battery status not available on this board";
                    });

    AddTool(
        "self.set_alarm",
        "Set an alarm. For a repeating alarm, provide 'days_of_week' as a comma-separated string "
        "(0=Sun, 1=Mon ... 6=Sat, e.g. '1,2,3,4,5' for weekdays). Default is everyday. "
        "For a one-time alarm on a specific date, provide 'year' (e.g. 2026), 'month' (1-12), and "
        "'day' (1-31). Do not provide days_of_week for specific date alarms.",
        PropertyList({Property("hour", kPropertyTypeInteger, 0, 23),
                      Property("minute", kPropertyTypeInteger, 0, 59),
                      Property("days_of_week", kPropertyTypeString, std::string("0,1,2,3,4,5,6")),
                      Property("year", kPropertyTypeInteger, -1),
                      Property("month", kPropertyTypeInteger, -1),
                      Property("day", kPropertyTypeInteger, -1)}),
        [](const PropertyList& properties) -> ReturnValue {
            int hour = properties["hour"].value<int>();
            int minute = properties["minute"].value<int>();
            int year = properties["year"].value<int>();
            int month = properties["month"].value<int>();
            int day = properties["day"].value<int>();

            uint8_t days_mask = 0;
            std::string d_str = properties["days_of_week"].value<std::string>();
            for (char c : d_str) {
                if (c >= '0' && c <= '6') {
                    days_mask |= (1 << (c - '0'));
                }
            }
            if (days_mask == 0)
                days_mask = 0x7F;  // Fallback

            ClockManager::GetInstance().AddAlarm(hour, minute, days_mask, year, month, day);
            return "Alarm set successfully";
        });

    AddTool("self.set_alarm_sound",
            "Set the alarm sound. Provide sound_id (1 or 2). Defaults to 1.",
            PropertyList({Property("sound_id", kPropertyTypeInteger, 1, 2)}),
            [](const PropertyList& properties) -> ReturnValue {
                int sound_id = properties["sound_id"].value<int>();
                ClockManager::GetInstance().SetAlarmSoundId(sound_id);
                return "Alarm sound set successfully";
            });

    AddTool("self.preview_alarm_sound",
            "Play a preview of the alarm sound. Provide sound_id (1, 2).",
            PropertyList({Property("sound_id", kPropertyTypeInteger, 1, 2)}),
            [](const PropertyList& properties) -> ReturnValue {
                int sound_id = properties["sound_id"].value<int>();

                if (sound_id == 2) {
                    Application::GetInstance().PlaySound(Lang::Sounds::OGG_ALARM2);
                }
                // else if (sound_id == 3) {
                //     Application::GetInstance().PlaySound(Lang::Sounds::OGG_ALARM3);
                // }
                else {
                    Application::GetInstance().PlaySound(Lang::Sounds::OGG_ALARM1);
                }

                return "Playing alarm sound preview.";
            });

    AddTool(
        "self.play_radio",
        "Play an internet radio stream (MP3/OGG). Provide the stream URL. "
        "Examples:\n"
        "- Ha Noi Community Radio: https://ha-noi-community-radio.radiocult.fm/stream\n"
        "- Lofi Hip Hop: http://stream.zeno.fm/f3wvbbqmdg8uv\n"
        "- Nhạc Trữ Tình (Bolero/Romantic): https://stream.zeno.fm/4q7y9hvkp2zuv\n"
        "- Radio Zeno 2: http://stream.zeno.fm/yvzaxu2qe7duv\n"
        "- V-Pop: http://stream.zeno.fm/5cyfrgpebkhvv\n"
        "If the user asks for a specific station, you can use these or provide your own MP3 URL.",
        PropertyList({Property("url", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ReturnValue {
            std::string url = properties["url"].value<std::string>();
            Application::GetInstance().StartRadio(url);
            return "Started playing radio stream.";
        });

    AddTool("self.stop_radio", "Stop the currently playing radio stream.", PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                Application::GetInstance().StopRadio();
                return "Stopped radio stream.";
            });

    AddTool(
        "self.search_radio",
        "Search for internet radio stations globally using Radio-Browser API. "
        "Returns a JSON list of MP3 streams matching the keyword. "
        "Use this tool when the user asks for a specific station (e.g. 'V-Pop', 'Hanoi', 'Jazz').",
        PropertyList({Property("keyword", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ReturnValue {
            std::string keyword = properties["keyword"].value<std::string>();
            std::string encoded = keyword;
            for (size_t i = 0; i < encoded.length(); ++i) {
                if (encoded[i] == ' ')
                    encoded[i] = '+';
            }

            std::string url =
                "https://de1.api.radio-browser.info/json/stations/search?name=" + encoded +
                "&limit=3&codec=MP3";
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
            if (http) {
                http->SetHeader("User-Agent", "XiaoZhi/1.0");
                http->SetHeader("Accept", "application/json");
                http->SetTimeout(5000);
                if (http->Open("GET", url)) {
                    std::string response;
                    char buffer[512];
                    while (true) {
                        int len = http->Read(buffer, sizeof(buffer) - 1);
                        if (len <= 0)
                            break;
                        buffer[len] = '\0';
                        response += buffer;
                        // Limit response size to prevent OOM
                        if (response.length() > 4096)
                            break;
                    }
                    return response.empty() ? "No results found" : response;
                }
            }
            return "Failed to search radio stations.";
        });

    AddTool("self.set_alarm_volume",
            "Set the independent volume for alarms (0 to 100). Default is 100.",
            PropertyList({Property("volume", kPropertyTypeInteger, 0, 100)}),
            [](const PropertyList& properties) -> ReturnValue {
                int volume = properties["volume"].value<int>();
                ClockManager::GetInstance().SetAlarmVolume(volume);
                return "Alarm volume set successfully";
            });

    AddTool("self.set_weather_location", "Set the latitude and longitude for weather forecasting.",
            PropertyList({Property("latitude", kPropertyTypeString),
                          Property("longitude", kPropertyTypeString)}),
            [](const PropertyList& properties) -> ReturnValue {
                try {
                    float lat = std::stof(properties["latitude"].value<std::string>());
                    float lon = std::stof(properties["longitude"].value<std::string>());
                    WeatherManager::GetInstance().SetLocation(lat, lon);
                    return "Weather location updated successfully";
                } catch (const std::exception& e) {
                    return "Invalid latitude or longitude";
                }
            });

    AddTool("self.temperature_alert.turn_on", "Turn on the high temperature voice alert",
            PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                Sht30Sensor::SetAlertEnabled(true);
                return "Temperature alert enabled";
            });

    AddTool("self.temperature_alert.turn_off",
            "Turn off the high temperature voice alert. Call this if the user asks you to stop "
            "reminding them about the room temperature.",
            PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                Sht30Sensor::SetAlertEnabled(false);
                return "Temperature alert disabled";
            });

    AddTool("self.list_alarms",
            "Get a list of all currently scheduled alarms. Returns an array of alarm objects with "
            "id, hour, minute, days_of_week (bitmask), year, month, and day.",
            PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                auto alarms = ClockManager::GetInstance().GetAlarms();
                cJSON* root = cJSON_CreateArray();
                for (const auto& alarm : alarms) {
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
                return root;
            });

    AddTool("self.remove_alarm",
            "Cancel or remove a specific alarm by its ID. You should list alarms first to find the "
            "correct ID.",
            PropertyList({Property("id", kPropertyTypeInteger)}),
            [](const PropertyList& properties) -> ReturnValue {
                int id = properties["id"].value<int>();
                ClockManager::GetInstance().RemoveAlarm(id);
                return "Alarm removed successfully";
            });

    AddTool("self.clear_all_alarms", "Cancel and remove all scheduled alarms.", PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                auto alarms = ClockManager::GetInstance().GetAlarms();
                for (const auto& alarm : alarms) {
                    ClockManager::GetInstance().RemoveAlarm(alarm.id);
                }
                return "All alarms removed successfully";
            });

    AddUserOnlyTool("self.reboot", "Reboot the system", PropertyList(),
                    [this](const PropertyList& properties) -> ReturnValue {
                        auto& app = Application::GetInstance();
                        app.Schedule([&app]() {
                            ESP_LOGW(TAG, "User requested reboot");
                            vTaskDelay(pdMS_TO_TICKS(1000));

                            app.Reboot();
                        });
                        return true;
                    });

    // Firmware upgrade
    AddUserOnlyTool("self.upgrade_firmware",
                    "Upgrade firmware from a specific URL. This will download and install the "
                    "firmware, then reboot the device.",
                    PropertyList({Property("url", kPropertyTypeString)}),
                    [this](const PropertyList& properties) -> ReturnValue {
                        auto url = properties["url"].value<std::string>();
                        ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s", url.c_str());

                        auto& app = Application::GetInstance();
                        app.Schedule([url, &app]() {
                            bool success = app.UpgradeFirmware(url);
                            if (!success) {
                                ESP_LOGE(TAG, "Firmware upgrade failed");
                            }
                        });

                        return true;
                    });

    // Display control
#ifdef HAVE_LVGL
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display) {
        AddUserOnlyTool("self.screen.get_info",
                        "Information about the screen, including width, height, etc.",
                        PropertyList(), [display](const PropertyList& properties) -> ReturnValue {
                            cJSON* json = cJSON_CreateObject();
                            cJSON_AddNumberToObject(json, "width", display->width());
                            cJSON_AddNumberToObject(json, "height", display->height());
                            if (dynamic_cast<OledDisplay*>(display)) {
                                cJSON_AddBoolToObject(json, "monochrome", true);
                            } else {
                                cJSON_AddBoolToObject(json, "monochrome", false);
                            }
                            return json;
                        });

#if CONFIG_LV_USE_SNAPSHOT
        AddUserOnlyTool(
            "self.screen.snapshot", "Snapshot the screen and upload it to a specific URL",
            PropertyList({Property("url", kPropertyTypeString),
                          Property("quality", kPropertyTypeInteger, 80, 1, 100)}),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto quality = properties["quality"].value<int>();

                std::string jpeg_data;
                if (!display->SnapshotToJpeg(jpeg_data, quality)) {
                    throw std::runtime_error("Failed to snapshot screen");
                }

                ESP_LOGI(TAG, "Upload snapshot %u bytes to %s", jpeg_data.size(), url.c_str());

                // 构造multipart/form-data请求体
                std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";

                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                if (!http->Open("POST", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                {
                    // 文件字段头部
                    std::string file_header;
                    file_header += "--" + boundary + "\r\n";
                    file_header +=
                        "Content-Disposition: form-data; name=\"file\"; "
                        "filename=\"screenshot.jpg\"\r\n";
                    file_header += "Content-Type: image/jpeg\r\n";
                    file_header += "\r\n";
                    http->Write(file_header.c_str(), file_header.size());
                }

                // JPEG数据
                http->Write((const char*)jpeg_data.data(), jpeg_data.size());

                {
                    // multipart尾部
                    std::string multipart_footer;
                    multipart_footer += "\r\n--" + boundary + "--\r\n";
                    http->Write(multipart_footer.c_str(), multipart_footer.size());
                }
                http->Write("", 0);

                if (http->GetStatusCode() != 200) {
                    throw std::runtime_error("Unexpected status code: " +
                                             std::to_string(http->GetStatusCode()));
                }
                std::string result = http->ReadAll();
                http->Close();
                ESP_LOGI(TAG, "Snapshot screen result: %s", result.c_str());
                return true;
            });

        AddUserOnlyTool(
            "self.screen.preview_image", "Preview an image on the screen",
            PropertyList({Property("url", kPropertyTypeString)}),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

                if (!http->Open("GET", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                int status_code = http->GetStatusCode();
                if (status_code != 200) {
                    throw std::runtime_error("Unexpected status code: " +
                                             std::to_string(status_code));
                }

                size_t content_length = http->GetBodyLength();
                char* data = (char*)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
                if (data == nullptr) {
                    throw std::runtime_error("Failed to allocate memory for image: " + url);
                }
                size_t total_read = 0;
                while (total_read < content_length) {
                    int ret = http->Read(data + total_read, content_length - total_read);
                    if (ret < 0) {
                        heap_caps_free(data);
                        throw std::runtime_error("Failed to download image: " + url);
                    }
                    if (ret == 0) {
                        break;
                    }
                    total_read += ret;
                }
                http->Close();

                auto image = std::make_unique<LvglAllocatedImage>(data, content_length);
                display->SetPreviewImage(std::move(image));
                return true;
            });
#endif  // CONFIG_LV_USE_SNAPSHOT
    }
#endif  // HAVE_LVGL

    // Assets download url (always registered — Settings storage works regardless of partition
    // layout)
    AddUserOnlyTool("self.assets.set_download_url", "Set the download url for the assets",
                    PropertyList({Property("url", kPropertyTypeString)}),
                    [](const PropertyList& properties) -> ReturnValue {
                        auto url = properties["url"].value<std::string>();
                        Settings settings("assets", true);
                        settings.SetString("download_url", url);
                        return true;
                    });
}

void McpServer::AddTool(McpTool* tool) {
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) {
            return t->name() == tool->name();
        }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string& name, const std::string& description,
                        const PropertyList& properties,
                        std::function<ReturnValue(const PropertyList&)> callback) {
    AddTool(new McpTool(name, description, properties, callback));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description,
                                const PropertyList& properties,
                                std::function<ReturnValue(const PropertyList&)> callback) {
    auto tool = new McpTool(name, description, properties, callback);
    tool->set_user_only(false);
    AddTool(tool);
}

void McpServer::ParseMessage(const std::string& message) {
    cJSON* json = cJSON_Parse(message.c_str());
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            auto camera = Board::GetInstance().GetCamera();
            if (camera) {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token)) {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON* json) {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) ||
        strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }

    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }

    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }

    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;

    if (method_str == "initialize") {
        if (cJSON_IsObject(params)) {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities)) {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message =
            "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{"
            "\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools)) {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments);
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::ReplyError(int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools) {
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";

    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";

    while (it != tools_.end()) {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only()) {
            ++it;
            continue;
        }

        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }

        json += tool_json;
        ++it;
    }

    if (json.back() == ',') {
        json.pop_back();
    }

    if (json.back() == '[' && !tools_.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit",
                 next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }

    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), [&tool_name](const McpTool* tool) {
        return tool->name() == tool_name;
    });

    if (tool_iter == tools_.end()) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    try {
        for (auto& argument : arguments) {
            bool found = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                    argument.set_value<int>(value->valueint);
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }

            if (!argument.has_default_value() && !found) {
                ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "tools/call: %s", e.what());
        ReplyError(id, e.what());
        return;
    }

    // Use main thread to call the tool
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool_iter, arguments = std::move(arguments)]() {
        try {
            ReplyResult(id, (*tool_iter)->Call(arguments));
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "tools/call: %s", e.what());
            ReplyError(id, e.what());
        }
    });
}
