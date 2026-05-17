// ============================================================================
// VL53L5CX Time-of-Flight sensor demo on ESP32
// ----------------------------------------------------------------------------
// This program talks to a VL53L5CX sensor, which is a tiny chip that shoots
// out invisible infrared laser pulses and measures how long they take to
// bounce back ("time of flight"). From that timing, it calculates distance.
// The cool part: it doesn't just measure one point — it measures an 8x8 grid
// of 64 distances at once, giving us a low-resolution "depth image."
//
// We're running this on an ESP32 microcontroller using ESP-IDF (Espressif's
// official C framework) and FreeRTOS (a tiny operating system that lets the
// chip juggle multiple tasks). The sensor talks to the ESP32 over I2C, a
// simple 2-wire protocol used everywhere in embedded electronics.
// ============================================================================

#include <stdio.h>             // printf() — standard C console output
#include "freertos/FreeRTOS.h" // Core FreeRTOS types (tick counts, etc.)
#include "freertos/task.h"     // vTaskDelay() — sleep without blocking other tasks
#include "esp_log.h"           // ESP_LOGI/E — nicely formatted log output over serial
#include "esp_err.h"           // ESP_ERROR_CHECK — aborts the program on a hardware error
#include "driver/i2c_master.h" // The new ESP-IDF v5 I2C driver (master-mode API)
#include "vl53l5cx_api.h"      // ST's official driver for the sensor

// "Tag" used by the logging system. Every ESP_LOGI/ESP_LOGE line gets
// prefixed with this so you can filter logs by subsystem in the serial monitor.
static const char *TAG = "vl53l5cx";

// ----------------------------------------------------------------------------
// Hardware wiring configuration
// ----------------------------------------------------------------------------
// I2C uses exactly two wires: SDA (data) and SCL (clock). You pick which
// physical pins on the ESP32 to use for them. GPIO 21 and 22 are the classic
// defaults on most ESP32 dev boards — easy to remember and broken out on
// nearly every breakout board.
#define SENSOR_SDA_GPIO GPIO_NUM_21
#define SENSOR_SCL_GPIO GPIO_NUM_22

// I2C bus speed in Hertz. 400 kHz ("fast mode") is the sweet spot: fast
// enough to stream depth frames at 10+ Hz, slow enough to be reliable on
// jumper wires and breadboards. The sensor supports up to 1 MHz on a clean
// PCB, but 400k is the safe default.
#define SENSOR_I2C_FREQ 400000

// app_main() is the entry point on ESP-IDF — equivalent to main() on a PC.
// The framework calls it automatically once the chip has booted and FreeRTOS
// is up and running.
void app_main(void)
{
    // Big "context" struct the ST driver uses to remember everything about
    // this sensor instance: its I2C handle, address, internal state, buffers,
    // etc. We pass a pointer to it into every API call.
    VL53L5CX_Configuration Dev;

    // Where the sensor will write each completed depth frame. It contains
    // the 64-element distance array and a bunch of optional metadata
    // (signal strength, target status, etc.) that we don't use here.
    VL53L5CX_ResultsData Results;

    // Small scratch variables for return codes and ready/alive flags.
    // The ST driver returns 0 on success and non-zero on failure.
    uint8_t status, isAlive, isReady;

    // ------------------------------------------------------------------------
    // Step 1: Set up the I2C bus
    // ------------------------------------------------------------------------
    // ESP-IDF v5 split I2C into two layers: first you create a "bus" (the
    // physical SDA/SCL pair), then you attach one or more "devices" to it.
    // This is nicer than the old API because multiple chips can share the
    // same two wires — each with its own address — and the driver tracks
    // them independently.
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,    // Use the default internal clock source
        .i2c_port = I2C_NUM_0,                // The ESP32 has two I2C peripherals; use port 0
        .sda_io_num = SENSOR_SDA_GPIO,        // Tell the driver which pin is SDA
        .scl_io_num = SENSOR_SCL_GPIO,        // ...and which is SCL
        .glitch_ignore_cnt = 7,               // Filter out tiny electrical noise spikes (7 is the recommended default)
        .flags.enable_internal_pullup = true, // I2C requires pull-up resistors; the ESP32 has weak built-in ones we can enable here. For long wires or production hardware you should add stronger external pull-ups (typically 4.7k ohm) instead.
    };
    i2c_master_bus_handle_t bus_handle;

    // Actually create the bus. ESP_ERROR_CHECK is a macro that aborts the
    // whole program if the call fails — fine here because nothing else can
    // work if I2C didn't come up.
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));

    // ------------------------------------------------------------------------
    // Step 2: Tell the ST driver how to reach our sensor
    // ------------------------------------------------------------------------
    // The rjrp44 port of ST's driver expects us to fill in a small "platform"
    // struct so its internal read/write functions know which bus and address
    // to use. Different ports of the driver (Arduino, STM32, ESP32) each have
    // their own version of this struct.
    Dev.platform.bus_config = bus_config;
    Dev.platform.address = VL53L5CX_DEFAULT_I2C_ADDRESS; // 0x52 — the factory-default 8-bit address

    // Now register the sensor as a device on the bus we just created.
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,               // I2C addresses are 7 bits on the wire (the 8th bit is read/write)
        .device_address = VL53L5CX_DEFAULT_I2C_ADDRESS >> 1, // ST defines the address as the 8-bit form; the ESP-IDF API wants the 7-bit form, so shift right by 1
        .scl_speed_hz = SENSOR_I2C_FREQ,                     // Clock speed for this specific device (each device on the bus can have its own speed)
    };
    // Add the device. The resulting handle is stored inside Dev.platform so
    // the ST driver can grab it whenever it needs to talk to the chip.
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_cfg, &Dev.platform.handle));

    // ------------------------------------------------------------------------
    // Step 3: Confirm the sensor is physically responding
    // ------------------------------------------------------------------------
    // Before doing anything heavy, ping the sensor and check it answers with
    // the expected device ID. This catches the most common bring-up problems
    // (wrong wiring, bad solder joint, sensor not powered) right away with a
    // clear error instead of mysterious failures later.
    ESP_LOGI(TAG, "Checking if sensor is alive...");
    status = vl53l5cx_is_alive(&Dev, &isAlive);
    if (!isAlive || status)
    {
        ESP_LOGE(TAG, "VL53L5CX not detected (status=%d, alive=%d)", status, isAlive);
        return; // Bail out — no point continuing if the sensor isn't there
    }
    ESP_LOGI(TAG, "Sensor is alive.");

    // ------------------------------------------------------------------------
    // Step 4: Upload the sensor's firmware
    // ------------------------------------------------------------------------
    // This is the surprising part: the VL53L5CX has no firmware in its own
    // flash. Every time it powers on, the host (us) has to push ~80 KB of
    // firmware to it over I2C before it can do anything. That's why this
    // step takes a couple of seconds — we're literally streaming an entire
    // program into the chip's RAM.
    ESP_LOGI(TAG, "Uploading firmware (~2-3 seconds)...");
    status = vl53l5cx_init(&Dev);
    if (status)
    {
        ESP_LOGE(TAG, "vl53l5cx_init failed with status %d", status);
        return;
    }
    ESP_LOGI(TAG, "Sensor initialized.");

    // ------------------------------------------------------------------------
    // Step 5: Configure how the sensor will measure
    // ------------------------------------------------------------------------
    // 8x8 = 64 zones across the field of view, vs. the lower-resolution 4x4
    // mode (16 zones). 8x8 gives us a more detailed depth image but caps the
    // maximum framerate at 15 Hz (vs. 60 Hz at 4x4).
    vl53l5cx_set_resolution(&Dev, VL53L5CX_RESOLUTION_8X8);

    // How many full 64-zone frames the sensor produces per second. 10 Hz is
    // a comfortable rate: fast enough to track a person walking through a
    // doorway, slow enough to leave plenty of headroom for the I2C transfer
    // and our processing.
    vl53l5cx_set_ranging_frequency_hz(&Dev, 10);

    // Tell the sensor to begin its continuous-ranging mode. From here on it
    // will fire its lasers and prepare new frames in the background; we just
    // poll for "is a frame ready?" and read it out when one is.
    vl53l5cx_start_ranging(&Dev);

    ESP_LOGI(TAG, "Ranging started. Streaming 8x8 depth grids...");

    // ------------------------------------------------------------------------
    // Step 6: Main loop — pull frames and print them
    // ------------------------------------------------------------------------
    int frame_count = 0;
    while (1)
    {
        // Ask the sensor: "do you have a new frame waiting for me?"
        // This is a cheap I2C read of a single status register.
        status = vl53l5cx_check_data_ready(&Dev, &isReady);

        if (isReady)
        {
            // Yes — pull the full frame (all 64 distances plus metadata)
            // into our Results struct over I2C.
            vl53l5cx_get_ranging_data(&Dev, &Results);
            frame_count++;

            // Print the frame as an 8x8 grid of millimeter distances.
            // The sensor returns the 64 zones in a flat array, row-major,
            // so index [row*8 + col] gives the cell at (row, col).
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

        // Sleep for 5 ms before polling again. vTaskDelay yields to FreeRTOS
        // so other tasks (Wi-Fi, logging, the idle task that resets the
        // watchdog) get to run — much better than a tight busy-loop, which
        // would hog the CPU and eventually trigger a watchdog reset.
        // At 10 Hz ranging a new frame arrives every 100 ms, so polling
        // every 5 ms means we pick it up almost immediately without burning
        // cycles.
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
