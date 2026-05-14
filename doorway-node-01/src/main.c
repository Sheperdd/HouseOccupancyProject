#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "vl53l5cx_api.h"

static const char *TAG = "vl53l5cx";

#define SENSOR_SDA_GPIO   GPIO_NUM_21
#define SENSOR_SCL_GPIO   GPIO_NUM_22
#define SENSOR_I2C_FREQ   400000

void app_main(void)
{
    VL53L5CX_Configuration Dev;
    VL53L5CX_ResultsData Results;
    uint8_t status, isAlive, isReady;

    // Build the I2C master bus. The rjrp44 library (v4.x) requires the user
    // to create the bus and add the sensor as a device — pin/freq config is
    // not done via menuconfig.
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = SENSOR_SDA_GPIO,
        .scl_io_num = SENSOR_SCL_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));

    Dev.platform.bus_config = bus_config;
    Dev.platform.address    = VL53L5CX_DEFAULT_I2C_ADDRESS;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = VL53L5CX_DEFAULT_I2C_ADDRESS >> 1,
        .scl_speed_hz    = SENSOR_I2C_FREQ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_cfg, &Dev.platform.handle));

    ESP_LOGI(TAG, "Checking if sensor is alive...");
    status = vl53l5cx_is_alive(&Dev, &isAlive);
    if (!isAlive || status)
    {
        ESP_LOGE(TAG, "VL53L5CX not detected (status=%d, alive=%d)", status, isAlive);
        return;
    }
    ESP_LOGI(TAG, "Sensor is alive.");

    ESP_LOGI(TAG, "Uploading firmware (~2-3 seconds)...");
    status = vl53l5cx_init(&Dev);
    if (status)
    {
        ESP_LOGE(TAG, "vl53l5cx_init failed with status %d", status);
        return;
    }
    ESP_LOGI(TAG, "Sensor initialized.");

    vl53l5cx_set_resolution(&Dev, VL53L5CX_RESOLUTION_8X8);
    vl53l5cx_set_ranging_frequency_hz(&Dev, 10);
    vl53l5cx_start_ranging(&Dev);

    ESP_LOGI(TAG, "Ranging started. Streaming 8x8 depth grids...");

    int frame_count = 0;
    while (1)
    {
        status = vl53l5cx_check_data_ready(&Dev, &isReady);

        if (isReady)
        {
            vl53l5cx_get_ranging_data(&Dev, &Results);
            frame_count++;

            printf("\n--- Frame %d ---\n", frame_count);
            for (int row = 0; row < 8; row++)
            {
                for (int col = 0; col < 8; col++)
                {
                    int idx = row * 8 + col;
                    printf("%5d ", Results.distance_mm[idx]);
                }
                printf("\n");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
