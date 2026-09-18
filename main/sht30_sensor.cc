#include "sht30_sensor.h"
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "application.h"
#include "settings.h"

#define SHT30_ADDR 0x44
#define SHT30_SCL_PIN GPIO_NUM_18
#define SHT30_SDA_PIN GPIO_NUM_17

static const char* TAG = "SHT30";
static i2c_master_bus_handle_t i2c_bus_ = nullptr;
static i2c_master_dev_handle_t sht30_dev_ = nullptr;
static bool alert_enabled_ = true;

void Sht30Sensor::SetAlertEnabled(bool enabled) {
    alert_enabled_ = enabled;
    Settings settings("sht30", true);
    settings.SetInt("alert_enabled", alert_enabled_ ? 1 : 0);
    ESP_LOGI(TAG, "Temperature alert enabled: %d", alert_enabled_);
}

esp_err_t Sht30Sensor::Initialize() {
    if (i2c_bus_ != nullptr)
        return ESP_OK;  // Already initialized

    Settings settings("sht30", false);
    alert_enabled_ = settings.GetInt("alert_enabled", 1) == 1;

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = -1;
    bus_cfg.sda_io_num = SHT30_SDA_PIN;
    bus_cfg.scl_io_num = SHT30_SCL_PIN;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &i2c_bus_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2C bus: %s", esp_err_to_name(err));
        return err;
    }



    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = SHT30_ADDR;
    dev_cfg.scl_speed_hz = 100000; // Restore to 100kHz

    err = i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &sht30_dev_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SHT30 device: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SHT30 I2C bus and device initialized");

        // --- SHT30 SOFT RESET ---
        uint8_t reset_cmd[2] = {0x30, 0xA2};
        esp_err_t reset_err = i2c_master_transmit(sht30_dev_, reset_cmd, sizeof(reset_cmd), 1000);
        if (reset_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send Soft Reset to SHT30: %s", esp_err_to_name(reset_err));
        } else {
            ESP_LOGI(TAG, "SHT30 Soft Reset successful.");
            vTaskDelay(pdMS_TO_TICKS(10)); // wait for reset to complete
        }

        xTaskCreate(
            [](void* arg) {
                TickType_t last_alert_tick = 0;
                while (true) {
                    vTaskDelay(pdMS_TO_TICKS(10000));  // Kiểm tra mỗi 10 giây
                    float temp = 0;
                    float hum = 0;
                    if (Sht30Sensor::Read(temp, hum) == ESP_OK) {
                        TickType_t current_tick = xTaskGetTickCount();
                        if (temp > 27.0f && alert_enabled_) {
                            // Alert if never alerted before, or if 5 minutes (300000 ms) have
                            // passed
                            if (last_alert_tick == 0 ||
                                (current_tick - last_alert_tick) > pdMS_TO_TICKS(300000)) {
                                auto state = Application::GetInstance().GetDeviceState();
                                if (state == kDeviceStateIdle || state == kDeviceStateListening) {
                                    char msg[32];
                                    int whole = (int)temp;
                                    int frac = (int)(temp * 10) % 10;
                                    snprintf(msg, sizeof(msg), "hot %d,%d C", whole, frac);
                                    Application::GetInstance().Chat(msg);
                                    // Make sure it doesn't stay 0 if current_tick is small
                                    last_alert_tick = current_tick == 0 ? 1 : current_tick;
                                }
                            }
                        } else if (temp <= 27.0f) {
                            last_alert_tick = 0;  // Reset alert timer when temperature drops
                        }
                    }
                }
            },
            "sht30_monitor", 4096, NULL, 1, NULL);
    }
    return err;
}

esp_err_t Sht30Sensor::Read(float& temperature, float& humidity) {
    if (!sht30_dev_) {
        ESP_LOGE(TAG, "SHT30 not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    int retries = 3;

    while (retries > 0) {
        // SHT30 command for high repeatability, clock stretching DISABLED
        uint8_t cmd[2] = {0x24, 0x00};
        err = i2c_master_transmit(sht30_dev_, cmd, sizeof(cmd), 1000);
        
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));  // wait for measurement
            uint8_t data[6];
            err = i2c_master_receive(sht30_dev_, data, sizeof(data), 1000);
            
            if (err == ESP_OK) {
                uint16_t raw_temp = (data[0] << 8) | data[1];
                uint16_t raw_hum = (data[3] << 8) | data[4];

                temperature = -45.0f + 175.0f * ((float)raw_temp / 65535.0f);
                humidity = 100.0f * ((float)raw_hum / 65535.0f);
                
                return ESP_OK; // Success
            }
        }
        
        ESP_LOGW(TAG, "SHT30 read failed: %s. Retries left: %d. Resetting...", esp_err_to_name(err), retries - 1);
        
        // Soft reset to recover the bus
        uint8_t reset_cmd[2] = {0x30, 0xA2};
        i2c_master_transmit(sht30_dev_, reset_cmd, sizeof(reset_cmd), 1000);
        vTaskDelay(pdMS_TO_TICKS(20));
        
        retries--;
    }

    ESP_LOGE(TAG, "SHT30 read failed permanently after retries.");
    return err;
}
