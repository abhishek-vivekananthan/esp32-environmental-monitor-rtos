#include "dht22.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#define TAG "DHT22"
#define DHT_TIMEOUT 1000

static int wait_for_level(gpio_num_t pin, int level, int timeout_us) {
    int elapsed = 0;
    while(gpio_get_level(pin) != level) {
        if(elapsed >= timeout_us) return -1;
        esp_rom_delay_us(1);
        elapsed++;
    }
    return elapsed;
}

esp_err_t dht22_read(gpio_num_t pin, float *temperature, float *humidity) {
    uint8_t data[5] = {0};

    // Send start signal
    gpio_set_direction(pin, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(pin, 0);
    vTaskDelay(20 / portTICK_PERIOD_MS);
    gpio_set_level(pin, 1);
    esp_rom_delay_us(30);
    gpio_set_direction(pin, GPIO_MODE_INPUT);

    // Wait for sensor response
    if(wait_for_level(pin, 0, DHT_TIMEOUT) < 0) return ESP_ERR_TIMEOUT;
    if(wait_for_level(pin, 1, DHT_TIMEOUT) < 0) return ESP_ERR_TIMEOUT;
    if(wait_for_level(pin, 0, DHT_TIMEOUT) < 0) return ESP_ERR_TIMEOUT;

    // Read 40 bits
    for(int i = 0; i < 40; i++) {
        if(wait_for_level(pin, 1, DHT_TIMEOUT) < 0) return ESP_ERR_TIMEOUT;
        int duration = wait_for_level(pin, 0, DHT_TIMEOUT);
        if(duration < 0) return ESP_ERR_TIMEOUT;
        data[i/8] <<= 1;
        if(duration > 40) data[i/8] |= 1;
    }

    // Verify checksum
    if(data[4] != ((data[0]+data[1]+data[2]+data[3]) & 0xFF)) {
        ESP_LOGE(TAG, "Checksum failed!");
        return ESP_ERR_INVALID_CRC;
    }

    // Convert raw data
    *humidity    = ((data[0] << 8) | data[1]) * 0.1f;
    *temperature = (((data[2] & 0x7F) << 8) | data[3]) * 0.1f;
    if(data[2] & 0x80) *temperature = -(*temperature);

    return ESP_OK;
}