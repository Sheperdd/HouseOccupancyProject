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
#include "esp_timer.h"         // esp_timer_get_time() — microsecond-resolution uptime for timestamps
#include "esp_system.h"        // esp_chip_info() — get info about the ESP32 chip we're running on
#include "driver/i2c_master.h" // The new ESP-IDF v5 I2C driver (master-mode API)
#include "vl53l5cx_api.h"      // ST's official driver for the sensor
#include "driver/uart.h"       // For UART configuration
#include "detection/tracker.h" // Phase 2 detector port (pure C; pulls in background.h)

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

    fflush(stdout);                                   // Make sure all our printf output goes out immediately, not buffered
    uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(10)); // Wait for any pending UART output to finish before we start logging
    uart_set_baudrate(UART_NUM_0, 921600);            // Speed up the serial output to 921600 baud (115200 is the default, but it can't keep up with 10+ FPS of depth frames at 8x8 resolution)

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
    // Step 6: Main loop — read frames, emit structured logs, track timing
    // ------------------------------------------------------------------------
    // Two output streams interleave on the serial port:
    //   FRAME,<seq>,<t_us>,<64 distances>,<64 statuses>
    //   STATS,<t_us>,<free_heap>,<min_heap>,<stack_hwm>,<avg_period_us>,
    //         <max_period_us>,<avg_read_us>
    // FRAME lines stream every ranging cycle; STATS lines are emitted once
    // per second and reset their measurement window.
    //
    // Why both? FRAME is the data we're capturing for offline analysis.
    // STATS lets us prove (or disprove) that the ESP32 has headroom for
    // Phase 2's detection logic and Phase 3's WiFi + MQTT load.

    int frame_count = 0;

    // ----- Phase 2 detector state -------------------------------------------
    // These structs are large (the background model carries a per-cell
    // bootstrap sample buffer), so they MUST NOT live on app_main's stack —
    // `static` puts them in BSS instead. The detector is pure C with no
    // hardware coupling; all it sees are the 64 distances + 64 statuses.
    static background_model_t bg;
    static tracker_t tk;
    bg_init(&bg);
    tracker_init(&tk);
    bool bg_ready = false;   // false until BOOTSTRAP_FRAMES empty frames seen
    int boot_frames = 0;     // bootstrap frames collected so far

    // Timing accumulators — all in microseconds, all reset each STATS window.
    int64_t last_frame_us = 0;       // Timestamp of the most recent completed frame
    int64_t last_stats_us = 0;       // When we last emitted a STATS line
    int64_t sum_frame_period_us = 0; // Sum of inter-frame intervals this window
    int64_t max_frame_period_us = 0; // Worst inter-frame interval this window
    int64_t sum_sensor_read_us = 0;  // Sum of sensor-read durations this window
    int periods_in_window = 0;       // Number of inter-frame intervals measured
    int reads_in_window = 0;         // Number of sensor reads (>= periods + 1)

    while (1)
    {
        status = vl53l5cx_check_data_ready(&Dev, &isReady);

        if (isReady)
        {
            // Bracket the I2C read with esp_timer to measure how long the
            // sensor takes to hand us a frame. At 8x8 / 10 Hz this is on
            // the order of 10-30ms — a meaningful chunk of our 100ms budget,
            // worth watching as we add more work to the loop in later phases.
            int64_t before_read = esp_timer_get_time();
            vl53l5cx_get_ranging_data(&Dev, &Results);
            int64_t after_read = esp_timer_get_time();
            frame_count++;

            // Update timing stats for this window.
            sum_sensor_read_us += (after_read - before_read);
            reads_in_window++;

            // Skip the period calculation on the very first frame after boot
            // (last_frame_us is still 0), so we don't record a bogus huge gap.
            if (last_frame_us != 0)
            {
                int64_t period = after_read - last_frame_us;
                sum_frame_period_us += period;
                if (period > max_frame_period_us)
                {
                    max_frame_period_us = period;
                }
                periods_in_window++;
            }
            last_frame_us = after_read;

            // ----- Emit FRAME line -----------------------------------------
            // Single line, comma-separated. printf is line-buffered by ESP-IDF's
            // stdio, so the '\n' at the end triggers a flush — the Python
            // capture script will see one whole line at a time, never partial.
            printf("FRAME,%d,%lld", frame_count, after_read);
            for (int i = 0; i < 64; i++)
            {
                printf(",%d", Results.distance_mm[i]);
            }
            for (int i = 0; i < 64; i++)
            {
                printf(",%d", Results.target_status[i]);
            }
            printf("\n");

            // ----- Detection pipeline --------------------------------------
            // Copy the sensor's frame into plain int arrays the detector
            // expects (it is hardware-agnostic: int dist[64] / int status[64]).
            int dist_i[64], status_i[64];
            for (int i = 0; i < 64; i++)
            {
                dist_i[i] = Results.distance_mm[i];
                status_i[i] = Results.target_status[i];
            }

            if (!bg_ready)
            {
                // Bootstrap: assume the doorway is empty for the first
                // BG_BOOTSTRAP_FRAMES frames and seed the background model.
                if (boot_frames == 0)
                {
                    ESP_LOGI(TAG, "Calibrating background — keep the doorway clear "
                                  "(%d frames / ~%ds)...",
                             BG_BOOTSTRAP_FRAMES, BG_BOOTSTRAP_FRAMES / 10);
                }
                bg_bootstrap_add(&bg, dist_i, status_i);
                boot_frames++;
                if (boot_frames >= BG_BOOTSTRAP_FRAMES)
                {
                    bg_bootstrap_finalize(&bg);
                    bg_ready = true;
                    ESP_LOGI(TAG, "Background calibrated (%d frames). Detection ready.",
                             boot_frames);
                }
            }
            else
            {
                // Steady state: perception -> largest blob -> tracker.
                bool occ[64];
                float dev[64];
                bg_process(&bg, dist_i, status_i, occ, dev);

                int cells[64];
                int blob_n = det_largest_blob(occ, cells);

                // esp_timer microseconds as the timestamp — no RTC until Phase 4
                // NTP; relative uptime is fine. t only flows into t_start/t_end.
                crossing_event_t ev = tracker_update(&tk, cells, blob_n, dev, after_read);
                if (ev.valid)
                {
                    const char *dir = (ev.direction == DIR_IN) ? "in"
                                    : (ev.direction == DIR_OUT) ? "out" : "none";
                    // EVENT,<esp_timer_us>,<direction>,<net>,<confidence>,<peak_blob>
                    printf("EVENT,%lld,%s,%.3f,%.3f,%d\n",
                           after_read, dir, ev.net, ev.confidence, ev.peak_blob);
                }
            }

            // ----- Emit STATS line once per second -------------------------
            if (after_read - last_stats_us >= 1000000)
            {
                // Averages over the just-completed window. Guard against
                // divide-by-zero on the very first window (one frame, zero
                // periods).
                int64_t avg_period = periods_in_window > 0
                                         ? sum_frame_period_us / periods_in_window
                                         : 0;
                int64_t avg_read = reads_in_window > 0
                                       ? sum_sensor_read_us / reads_in_window
                                       : 0;

                // esp_get_free_heap_size() = bytes available right now.
                // esp_get_minimum_free_heap_size() = lifetime watermark — the
                // lowest value free_heap has ever hit since boot. If this
                // drops over time, you have a memory leak somewhere.
                size_t free_heap = esp_get_free_heap_size();
                size_t min_free_heap = esp_get_minimum_free_heap_size();

                // uxTaskGetStackHighWaterMark(NULL) returns the closest this
                // task's stack has come to overflowing, in *words* (4 bytes
                // on ESP32). Multiply by 4 for bytes. NULL means "this task".
                UBaseType_t stack_hwm_words = uxTaskGetStackHighWaterMark(NULL);
                unsigned stack_hwm_bytes = (unsigned)stack_hwm_words * 4U;

                printf("STATS,%lld,%u,%u,%u,%lld,%lld,%lld\n",
                       after_read,
                       (unsigned)free_heap,
                       (unsigned)min_free_heap,
                       stack_hwm_bytes,
                       avg_period,
                       max_frame_period_us,
                       avg_read);

                // Reset the window. Note we deliberately leave last_frame_us
                // alone — the period from the last frame of this window to
                // the first frame of the next is a real measurement.
                sum_frame_period_us = 0;
                max_frame_period_us = 0;
                sum_sensor_read_us = 0;
                periods_in_window = 0;
                reads_in_window = 0;
                last_stats_us = after_read;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
