#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "dht22.h"
#include "oled.h"

// ── PIN DEFINITIONS ──────────────────────────────────
#define DHT_PIN      GPIO_NUM_4
#define MQ135_CH     ADC_CHANNEL_6
#define I2C_SDA      GPIO_NUM_21
#define I2C_SCL      GPIO_NUM_22
#define BUZZER_PIN   GPIO_NUM_27
#define LED_PIN      GPIO_NUM_26

// ── WI-FI CREDENTIALS ────────────────────────────────
#define WIFI_SSID     "Abhishek's S23"
#define WIFI_PASSWORD "12345678"

// ── HTTP / NODE-RED ENDPOINT ─────────────────────────
#define SERVER_URL    "http://10.243.209.227:1880/sensor"

// ── THRESHOLDS ───────────────────────────────────────
#define TEMP_LIMIT   35.0
#define AQ_LIMIT     2000

// ── SHARED DATA ──────────────────────────────────────
float g_temperature = 0;
float g_humidity    = 0;
int   g_airQuality  = 0;
SemaphoreHandle_t dataMutex;
SemaphoreHandle_t alertSemaphore;

// ── WI-FI EVENT HANDLER ──────────────────────────────
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data) {
    if(base == WIFI_EVENT &&
       event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI("WIFI", "Connecting...");
    }
    else if(base == WIFI_EVENT &&
            event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGW("WIFI", "Disconnected! Reconnecting...");
    }
    else if(base == IP_EVENT &&
            event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t*)data;
        ESP_LOGI("WIFI", "Connected! IP: " IPSTR,
                 IP2STR(&event->ip_info.ip));
    }
}

// ── WI-FI INITIALIZATION ─────────────────────────────
static void wifi_init(void) {
    esp_err_t ret = nvs_flash_init();
    if(ret == ESP_ERR_NVS_NO_FREE_PAGES ||
       ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    // Register event handlers
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                               &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               &wifi_event_handler, NULL);

    wifi_config_t wifi_config = {0};
    strcpy((char*)wifi_config.sta.ssid,     WIFI_SSID);
    strcpy((char*)wifi_config.sta.password, WIFI_PASSWORD);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    // Disable WiFi power-save: prevents radio sleep cycles from
    // disrupting DHT22's microsecond-timing bit-bang reads
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI("WIFI", "Wi-Fi init done! Waiting 6s...");
    vTaskDelay(6000 / portTICK_PERIOD_MS);
}

// ── TASK 1: Read DHT22 ───────────────────────────────
void task_readDHT(void *pvParam) {
    float temperature = 0;
    float humidity    = 0;
    vTaskDelay(2000 / portTICK_PERIOD_MS);

    while(1) {
        esp_err_t result = dht22_read(DHT_PIN,
                           &temperature, &humidity);
        if(result == ESP_OK) {
            ESP_LOGI("DHT22", "Temp: %.1fC | Hum: %.1f%%",
                     temperature, humidity);
            if(xSemaphoreTake(dataMutex, 100) == pdTRUE) {
                g_temperature = temperature;
                g_humidity    = humidity;
                xSemaphoreGive(dataMutex);
            }
            if(temperature > TEMP_LIMIT) {
                ESP_LOGW("DHT22", "HIGH TEMP! Alert!");
                xSemaphoreGive(alertSemaphore);
            }
        } else {
            ESP_LOGE("DHT22", "Read failed!");
        }
        vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
}

// ── TASK 2: Read MQ135 ───────────────────────────────
void task_readMQ135(void *pvParam) {
    adc_oneshot_unit_handle_t adc_handle;
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    adc_oneshot_new_unit(&init_config, &adc_handle);
    adc_oneshot_chan_cfg_t chan_config = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_oneshot_config_channel(adc_handle, MQ135_CH,
                               &chan_config);

    ESP_LOGI("MQ135", "Warming up 10 seconds...");
    vTaskDelay(10000 / portTICK_PERIOD_MS);

    while(1) {
        int rawValue = 0;
        adc_oneshot_read(adc_handle, MQ135_CH, &rawValue);
        ESP_LOGI("MQ135", "Air Quality: %d raw", rawValue);

        if(xSemaphoreTake(dataMutex, 100) == pdTRUE) {
            g_airQuality = rawValue;
            xSemaphoreGive(dataMutex);
        }
        if(rawValue > AQ_LIMIT) {
            ESP_LOGW("MQ135", "POOR AIR! Alert!");
            xSemaphoreGive(alertSemaphore);
        }
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}

// ── TASK 3: OLED Display ─────────────────────────────
void task_display(void *pvParam) {
    char line[22];
    vTaskDelay(3000 / portTICK_PERIOD_MS);

    while(1) {
        float temp = 0;
        float hum  = 0;
        int   aq   = 0;

        if(xSemaphoreTake(dataMutex, 100) == pdTRUE) {
            temp = g_temperature;
            hum  = g_humidity;
            aq   = g_airQuality;
            xSemaphoreGive(dataMutex);
        }

        char *aqLabel;
        if(aq < 1000)       aqLabel = "GOOD";
        else if(aq < 2000)  aqLabel = "MODERATE";
        else                aqLabel = "POOR!";

        oled_clear();
        oled_print(0, "ENV MONITOR");
        snprintf(line, sizeof(line), "Temp: %.1f C", temp);
        oled_print(2, line);
        snprintf(line, sizeof(line), "Hum:  %.1f %%", hum);
        oled_print(4, line);
        snprintf(line, sizeof(line), "Air:  %s", aqLabel);
        oled_print(6, line);

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

// ── TASK 4: Alert Manager ────────────────────────────
void task_alert(void *pvParam) {
    // Configure pins
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUZZER_PIN) | (1ULL << LED_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Explicitly keep buzzer OFF at start
    // Active buzzer: HIGH = OFF, LOW = ON
    // Try HIGH first — if still beeping, we swap logic below
    gpio_set_level(BUZZER_PIN, 1);  // Buzzer OFF
    gpio_set_level(LED_PIN,    0);  // LED OFF

    while(1) {
        if(xSemaphoreTake(alertSemaphore,
                          portMAX_DELAY) == pdTRUE) {
            ESP_LOGW("ALERT", "ALERT TRIGGERED!");

            // Beep 3 times
            for(int i = 0; i < 3; i++) {
                gpio_set_level(BUZZER_PIN, 0);  // Buzzer ON
                gpio_set_level(LED_PIN,    1);  // LED ON
                vTaskDelay(300 / portTICK_PERIOD_MS);
                gpio_set_level(BUZZER_PIN, 1);  // Buzzer OFF
                gpio_set_level(LED_PIN,    0);  // LED OFF
                vTaskDelay(200 / portTICK_PERIOD_MS);
            }
            ESP_LOGW("ALERT", "Alert done.");
        }
    }
}


// ── TASK 5: HTTP Publisher ───────────────────────────
// Sends sensor data to Node-RED via HTTP POST
void task_publish(void *pvParam) {
    char payload[128];

    // Wait for sensors to warm up first
    vTaskDelay(15000 / portTICK_PERIOD_MS);

    while(1) {
        float temp = 0;
        float hum  = 0;
        int   aq   = 0;

        if(xSemaphoreTake(dataMutex, 100) == pdTRUE) {
            temp = g_temperature;
            hum  = g_humidity;
            aq   = g_airQuality;
            xSemaphoreGive(dataMutex);
        }

        // Build JSON payload
        snprintf(payload, sizeof(payload),
            "{\"temperature\":%.1f,\"humidity\":%.1f,\"airquality\":%d}",
            temp, hum, aq);

        esp_http_client_config_t config = {
            .url    = SERVER_URL,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 5000,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);

        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, payload, strlen(payload));

        esp_err_t err = esp_http_client_perform(client);

        if(err == ESP_OK) {
            ESP_LOGI("HTTP", "Sent: %s", payload);
        } else {
            ESP_LOGE("HTTP", "Send failed: %s", esp_err_to_name(err));
        }

        esp_http_client_cleanup(client);

        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}

// ── TASK 6: Serial Monitor ───────────────────────────
void task_monitor(void *pvParam) {
    vTaskDelay(12000 / portTICK_PERIOD_MS);
    while(1) {
        if(xSemaphoreTake(dataMutex, 100) == pdTRUE) {
            float temp = g_temperature;
            float hum  = g_humidity;
            int   aq   = g_airQuality;
            xSemaphoreGive(dataMutex);
            ESP_LOGI("MONITOR", "=== Summary ===");
            ESP_LOGI("MONITOR", "Temp : %.1f C", temp);
            ESP_LOGI("MONITOR", "Hum  : %.1f%%", hum);
            ESP_LOGI("MONITOR", "AQ   : %d raw", aq);
            ESP_LOGI("MONITOR", "===============");
        }
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}

// ── MAIN ─────────────────────────────────────────────
void app_main(void) {
    ESP_LOGI("MAIN", "Environmental Monitor Starting...");

    // 1. Connect to Wi-Fi first
    wifi_init();

    // 2. Initialize I2C for OLED
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_SDA,
        .scl_io_num       = I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    i2c_param_config(I2C_NUM_0, &conf);
    i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);

    // 3. Initialize OLED
    if(oled_init() == ESP_OK) {
        oled_print(0, "ENV MONITOR");
        oled_print(2, "WiFi OK!");
        ESP_LOGI("MAIN", "OLED initialized!");
    } else {
        ESP_LOGE("MAIN", "OLED init failed!");
    }

    // 4. Create mutex and semaphore
    dataMutex      = xSemaphoreCreateMutex();
    alertSemaphore = xSemaphoreCreateBinary();

    // 5. Start all 6 tasks
    xTaskCreate(task_alert,     "AlertTask",   2048, NULL, 6, NULL);
    xTaskCreate(task_readDHT,   "DHTTask",     4096, NULL, 5, NULL);
    xTaskCreate(task_readMQ135, "MQ135Task",   4096, NULL, 5, NULL);
    xTaskCreate(task_display,   "DisplayTask", 4096, NULL, 4, NULL);
    xTaskCreate(task_publish,   "PublishTask", 4096, NULL, 3, NULL);
    xTaskCreate(task_monitor,   "MonitorTask", 2048, NULL, 2, NULL);

    ESP_LOGI("MAIN", "All 6 tasks started!");
}