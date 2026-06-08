// ============================================================================
// VL53L5CX Time-of-Flight sensor — doorway occupancy node
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
//
// POWER MODEL (Phase 2): the node runs as a two-tier state machine —
//   CALIBRATING : continuous ranging; seed the background (doorway must be clear)
//   ARMED       : autonomous ranging + motion thresholds; the sensor watches on
//                 its own and pulses INT when motion crosses a threshold. This is
//                 the low-power "wake" tier (where the ESP32 will sleep).
//   CAPTURING   : continuous 10 Hz ranging; run the full detector to get a
//                 crossing + direction, until the doorway clears, then re-arm.
// The wake tier only signals "something moved"; the detector needs the dense
// continuous stream, so the actual crossing is always captured in CAPTURING.
// ============================================================================

#include <stdio.h>                                // printf() — standard C console output
#include <string.h>                               // memset() — zero the thresholds array
#include "freertos/FreeRTOS.h"                    // Core FreeRTOS types (tick counts, etc.)
#include "freertos/task.h"                        // vTaskDelay() — sleep without blocking other tasks
#include "freertos/queue.h"                       // FreeRTOS queue — the ISR->task hand-off
#include "esp_log.h"                              // ESP_LOGI/E — nicely formatted log output over serial
#include "esp_err.h"                              // ESP_ERROR_CHECK — aborts the program on a hardware error
#include "esp_timer.h"                            // esp_timer_get_time() — microsecond-resolution uptime for timestamps
#include "esp_system.h"                           // esp_chip_info() — get info about the ESP32 chip we're running on
#include "driver/i2c_master.h"                    // The new ESP-IDF v5 I2C driver (master-mode API)
#include "vl53l5cx_api.h"                         // ST's official driver for the sensor
#include "vl53l5cx_plugin_motion_indicator.h"     // Motion indicator plugin (autonomous wake)
#include "vl53l5cx_plugin_detection_thresholds.h" // Detection thresholds plugin (autonomous wake)
#include "driver/uart.h"                          // For UART configuration
#include "driver/gpio.h"                          // GPIO read/config — for the sensor INT pin
#include "detection/tracker.h"                    // Phase 2 detector port (pure C; pulls in background.h)

// "Tag" used by the logging system. Every ESP_LOGI/ESP_LOGE line gets
// prefixed with this so you can filter logs by subsystem in the serial monitor.
static const char *TAG = "vl53l5cx";

// ----------------------------------------------------------------------------
// Hardware wiring configuration
// ----------------------------------------------------------------------------
// I2C uses exactly two wires: SDA (data) and SCL (clock). GPIO 21 and 22 are
// the classic defaults on most ESP32 dev boards.
#define SENSOR_SDA_GPIO GPIO_NUM_21
#define SENSOR_SCL_GPIO GPIO_NUM_22

// The sensor's INT pin (open-drain, active-low). GPIO 4 is RTC-capable (so it
// can wake the ESP32 from deep sleep later), not a strapping pin, and free.
// Held idle-high by the ESP32's INTERNAL pull-up (~45k, enabled in setup); the
// sensor pulls it LOW when a motion-threshold event fires. NOTE: internal
// pull-ups switch off in deep sleep — adding deep sleep later will require
// latching an RTC pull-up on this pin (rtc_gpio_pullup_en); light sleep keeps it.
#define SENSOR_INT_GPIO GPIO_NUM_4

// I2C bus speed in Hertz. 400 kHz ("fast mode") is the sweet spot.
#define SENSOR_I2C_FREQ 400000

// ---- two-tier mode parameters ---------------------------------------------
#define VL53L5CX_THRESHOLD_EVENT 1   // token the ISR pushes into the queue
#define MOTION_THRESHOLD       44    // motion-indicator level that counts as movement
                                     //   (ST's example value; tune for your doorway)
#define CAPTURE_FREQ_HZ        10    // continuous ranging rate while capturing a crossing
#define ARMED_FREQ_HZ          5     // autonomous ranging rate while waiting for motion
#define ARMED_INTEGRATION_MS   10    // integration time in autonomous mode
#define CAPTURE_IDLE_FRAMES    20    // re-arm after this many consecutive clear frames (~2s @10Hz)
#define CAPTURE_MAX_FRAMES     300   // safety cap on one capture burst (~30s @10Hz)

// Node operating tiers (see file header).
typedef enum { STATE_CALIBRATING, STATE_ARMED, STATE_CAPTURING } node_state_t;

// Filled by the ISR, drained by the main task. One word per event.
static QueueHandle_t vl53l5cx_queue = NULL;

// INT interrupt handler. Must be tiny and IRAM-resident: it does NO I2C, NO
// printf — it just drops a token in the queue and returns. The slow work (the
// I2C read + detector) happens back in the task that receives from the queue.
void IRAM_ATTR vl53l5cx_isr_handler(void *arg)
{
    uint32_t event = (uint32_t)arg;
    xQueueSendFromISR(vl53l5cx_queue, &event, NULL);
}

// Switch the sensor to CONTINUOUS 8x8 ranging — the capture/processing tier.
// Thresholds are disabled so INT stays quiet while we poll frames over I2C.
// Returns 0 on success (ORed sensor status; stop_ranging status ignored since
// it's harmless to "stop" a sensor that isn't ranging yet).
static uint8_t sensor_start_continuous(VL53L5CX_Configuration *Dev)
{
    uint8_t s = 0;
    vl53l5cx_stop_ranging(Dev);
    s |= vl53l5cx_set_detection_thresholds_enable(Dev, 0);
    s |= vl53l5cx_set_resolution(Dev, VL53L5CX_RESOLUTION_8X8);
    s |= vl53l5cx_set_ranging_mode(Dev, VL53L5CX_RANGING_MODE_CONTINUOUS);
    s |= vl53l5cx_set_ranging_frequency_hz(Dev, CAPTURE_FREQ_HZ);
    s |= vl53l5cx_start_ranging(Dev);
    return s;
}

// Switch the sensor to AUTONOMOUS 8x8 with motion thresholds — the wake tier.
// Re-programs the motion indicator + thresholds every time so the wake config
// is always fresh after a mode switch, then enables the INT. Returns 0 on OK.
static uint8_t sensor_arm_autonomous(VL53L5CX_Configuration *Dev,
                                     VL53L5CX_Motion_Configuration *mc,
                                     VL53L5CX_DetectionThresholds *thr)
{
    uint8_t s = 0;
    vl53l5cx_stop_ranging(Dev);
    s |= vl53l5cx_set_resolution(Dev, VL53L5CX_RESOLUTION_8X8);
    s |= vl53l5cx_motion_indicator_init(Dev, mc, VL53L5CX_RESOLUTION_8X8);
    // Motion distance window 0.5–2.0 m (constraints: span < 1500 mm, min > 400 mm).
    s |= vl53l5cx_motion_indicator_set_distance_motion(Dev, mc, 500, 1999);
    s |= vl53l5cx_set_ranging_mode(Dev, VL53L5CX_RANGING_MODE_AUTONOMOUS);
    s |= vl53l5cx_set_ranging_frequency_hz(Dev, ARMED_FREQ_HZ);
    s |= vl53l5cx_set_integration_time_ms(Dev, ARMED_INTEGRATION_MS);
    s |= vl53l5cx_set_detection_thresholds(Dev, thr);
    s |= vl53l5cx_set_detection_thresholds_enable(Dev, 1);
    s |= vl53l5cx_start_ranging(Dev);
    return s;
}

// Emit a crossing event on the serial line.
//   EVENT,<esp_timer_us>,<direction>,<net>,<confidence>,<peak_blob>
static void emit_event(const crossing_event_t *e, int64_t t_us)
{
    const char *dir = (e->direction == DIR_IN)    ? "in"
                      : (e->direction == DIR_OUT) ? "out"
                                                  : "none";
    printf("EVENT,%lld,%s,%.3f,%.3f,%d\n",
           t_us, dir, e->net, e->confidence, e->peak_blob);
}

// app_main() is the entry point on ESP-IDF — equivalent to main() on a PC.
void app_main(void)
{
    // Big "context" struct the ST driver uses to remember everything about this
    // sensor instance: its I2C handle, address, internal state, buffers, etc.
    VL53L5CX_Configuration Dev;

    // Where the sensor writes each completed depth frame (64 distances + status).
    VL53L5CX_ResultsData Results;

    // Motion indicator config (zone map); programmed when we arm.
    VL53L5CX_Motion_Configuration motion_config;

    // Scratch for return codes / flags. The ST driver returns 0 on success.
    uint8_t status, isAlive, isReady;
    uint32_t event;

    fflush(stdout);                                   // Flush any buffered output before we reconfigure UART
    uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(10)); // Let pending UART output drain
    uart_set_baudrate(UART_NUM_0, 921600);            // 921600 baud keeps up with 10+ FPS of 8x8 frames

    // ------------------------------------------------------------------------
    // Step 1: Set up the I2C bus
    // ------------------------------------------------------------------------
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,    // Use the default internal clock source
        .i2c_port = I2C_NUM_0,                // The ESP32 has two I2C peripherals; use port 0
        .sda_io_num = SENSOR_SDA_GPIO,        // Tell the driver which pin is SDA
        .scl_io_num = SENSOR_SCL_GPIO,        // ...and which is SCL
        .glitch_ignore_cnt = 7,               // Filter out tiny electrical noise spikes
        .flags.enable_internal_pullup = true, // Weak built-in I2C pull-ups (add 4.7k externals for production)
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));

    // ------------------------------------------------------------------------
    // Step 2: Tell the ST driver how to reach our sensor
    // ------------------------------------------------------------------------
    Dev.platform.bus_config = bus_config;
    Dev.platform.address = VL53L5CX_DEFAULT_I2C_ADDRESS; // 0x52 — factory-default 8-bit address

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,               // 7-bit addresses on the wire
        .device_address = VL53L5CX_DEFAULT_I2C_ADDRESS >> 1, // ST gives the 8-bit form; API wants 7-bit
        .scl_speed_hz = SENSOR_I2C_FREQ,                     // Per-device clock speed
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_cfg, &Dev.platform.handle));

    // ------------------------------------------------------------------------
    // Step 3: Confirm the sensor is physically responding
    // ------------------------------------------------------------------------
    ESP_LOGI(TAG, "Checking if sensor is alive...");
    status = vl53l5cx_is_alive(&Dev, &isAlive);
    if (!isAlive || status)
    {
        ESP_LOGE(TAG, "VL53L5CX not detected (status=%d, alive=%d)", status, isAlive);
        return;
    }
    ESP_LOGI(TAG, "Sensor is alive.");

    // ------------------------------------------------------------------------
    // Step 4: Upload the sensor's firmware (~80 KB over I2C, takes 2-3 s)
    // ------------------------------------------------------------------------
    // The VL53L5CX has no flash of its own — the host streams firmware into its
    // RAM on every power-up before it can range.
    ESP_LOGI(TAG, "Uploading firmware (~2-3 seconds)...");
    status = vl53l5cx_init(&Dev);
    if (status)
    {
        ESP_LOGE(TAG, "vl53l5cx_init failed with status %d", status);
        return;
    }
    ESP_LOGI(TAG, "Sensor initialized.");

    // ------------------------------------------------------------------------
    // Step 5: INT pin + interrupt + detection-threshold programming (one-time)
    // ------------------------------------------------------------------------
    // INT pin: input with the ESP32's INTERNAL pull-up enabled (~45k, close to
    // the datasheet's 47k). It holds the line idle-high; the sensor pulls it LOW
    // on a motion-threshold event. (No external resistor anymore.)
    gpio_set_direction(SENSOR_INT_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(SENSOR_INT_GPIO, GPIO_PULLUP_ONLY);

    // INT -> ISR -> queue. The motion indicator + thresholds are (re)applied
    // inside sensor_arm_autonomous() each time we enter the wake tier.
    vl53l5cx_queue = xQueueCreate(10, sizeof(uint32_t));
    gpio_set_intr_type(SENSOR_INT_GPIO, GPIO_INTR_NEGEDGE);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(SENSOR_INT_GPIO, vl53l5cx_isr_handler, (void *)VL53L5CX_THRESHOLD_EVENT);

    // Detection thresholds: trip the INT when ANY zone's motion indicator
    // exceeds MOTION_THRESHOLD. static => kept off app_main's stack.
    static VL53L5CX_DetectionThresholds thresholds[VL53L5CX_NB_THRESHOLDS];
    memset(thresholds, 0, sizeof(thresholds));
    for (int i = 0; i < 64; i++)
    {
        thresholds[i].zone_num = i;
        thresholds[i].measurement = VL53L5CX_MOTION_INDICATOR;
        thresholds[i].type = VL53L5CX_GREATER_THAN_MAX_CHECKER;
        thresholds[i].mathematic_operation = VL53L5CX_OPERATION_NONE; // 4x4 only
        // GREATER_THAN_MAX: set low/high equal to avoid ambiguity over which
        // field the checker reads (ST's example uses 44 as "this is movement").
        thresholds[i].param_low_thresh = MOTION_THRESHOLD;
        thresholds[i].param_high_thresh = MOTION_THRESHOLD;
    }
    thresholds[63].zone_num |= VL53L5CX_LAST_THRESHOLD; // mark the final checker

    // ------------------------------------------------------------------------
    // Step 6: Detector state + two-tier state machine
    // ------------------------------------------------------------------------
    // static: the background model carries a large per-cell bootstrap buffer and
    // must not live on app_main's small stack.
    static background_model_t bg;
    static tracker_t tk;
    bg_init(&bg);
    tracker_init(&tk);
    int boot_frames = 0;

    node_state_t state = STATE_CALIBRATING;
    int empty_streak = 0;     // consecutive clear frames during a capture burst
    int capture_frames = 0;   // length of the current burst (safety bound)
    int max_blob = 0;         // diagnostic: peak largest-blob size seen this burst
    int events_in_burst = 0;  // diagnostic: events emitted this burst

    // Timing accumulators (microseconds) for the once-per-second STATS line,
    // emitted only while capturing.
    int frame_count = 0;
    int64_t last_frame_us = 0, last_stats_us = 0;
    int64_t sum_frame_period_us = 0, max_frame_period_us = 0, sum_sensor_read_us = 0;
    int periods_in_window = 0, reads_in_window = 0;

    // Boot straight into continuous ranging to calibrate the background.
    if (sensor_start_continuous(&Dev))
    {
        ESP_LOGE(TAG, "Failed to start continuous ranging");
        return;
    }
    ESP_LOGI(TAG, "Calibrating background — keep the doorway clear (%d frames / ~%ds)...",
             BG_BOOTSTRAP_FRAMES, BG_BOOTSTRAP_FRAMES / CAPTURE_FREQ_HZ);

    while (1)
    {
        // -------------------- ARMED: low-power wait for motion --------------
        if (state == STATE_ARMED)
        {
            // Block until the sensor's INT fires (motion crossed the threshold).
            // TODO(power): replace this blocking receive with light sleep + GPIO
            // wake so the ESP32 itself sleeps while the sensor watches.
            xQueueReceive(vl53l5cx_queue, &event, portMAX_DELAY);

            if (sensor_start_continuous(&Dev))
            {
                ESP_LOGE(TAG, "Failed to switch to continuous on wake");
            }
            state = STATE_CAPTURING;
            empty_streak = 0;
            capture_frames = 0;
            max_blob = 0;
            events_in_burst = 0;
            last_frame_us = 0;
            last_stats_us = esp_timer_get_time();
            ESP_LOGI(TAG, "Motion detected — capturing.");
            continue;
        }

        // -------------------- CALIBRATING / CAPTURING: read a frame ---------
        status = vl53l5cx_check_data_ready(&Dev, &isReady);
        if (!isReady)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int64_t before_read = esp_timer_get_time();
        vl53l5cx_get_ranging_data(&Dev, &Results);
        int64_t after_read = esp_timer_get_time();
        frame_count++;

        // Copy the frame into the detector's plain int arrays.
        int dist_i[64], status_i[64];
        for (int i = 0; i < 64; i++)
        {
            dist_i[i] = Results.distance_mm[i];
            status_i[i] = Results.target_status[i];
        }

        // FRAME line (offline capture / debugging).
        printf("FRAME,%d,%lld", frame_count, after_read);
        for (int i = 0; i < 64; i++) printf(",%d", Results.distance_mm[i]);
        for (int i = 0; i < 64; i++) printf(",%d", Results.target_status[i]);
        printf("\n");

        // -------------------- CALIBRATING -----------------------------------
        if (state == STATE_CALIBRATING)
        {
            bg_bootstrap_add(&bg, dist_i, status_i);
            boot_frames++;
            if (boot_frames >= BG_BOOTSTRAP_FRAMES)
            {
                bg_bootstrap_finalize(&bg);
                ESP_LOGI(TAG, "Background calibrated (%d frames). Arming.", boot_frames);
                if (sensor_arm_autonomous(&Dev, &motion_config, thresholds))
                {
                    ESP_LOGE(TAG, "Failed to arm autonomous mode");
                }
                xQueueReset(vl53l5cx_queue); // drop any events raised during setup
                state = STATE_ARMED;
            }
            continue;
        }

        // -------------------- CAPTURING -------------------------------------
        // Timing stats for the STATS line.
        sum_sensor_read_us += (after_read - before_read);
        reads_in_window++;
        if (last_frame_us != 0)
        {
            int64_t period = after_read - last_frame_us;
            sum_frame_period_us += period;
            if (period > max_frame_period_us) max_frame_period_us = period;
            periods_in_window++;
        }
        last_frame_us = after_read;

        // Perception -> largest blob -> tracker.
        bool occ[64];
        float dev[64];
        bg_process(&bg, dist_i, status_i, occ, dev);
        int cells[64];
        int blob_n = det_largest_blob(occ, cells);

        // esp_timer microseconds as the timestamp — no RTC until Phase 4 NTP.
        crossing_event_t ev = tracker_update(&tk, cells, blob_n, dev, after_read);
        if (ev.valid)
        {
            emit_event(&ev, after_read);
            events_in_burst++;
        }

        // Is the doorway clear this frame (no qualifying blob)?
        if (blob_n < TRK_MIN_BLOB_CELLS) empty_streak++;
        else empty_streak = 0;
        if (blob_n > max_blob) max_blob = blob_n;
        capture_frames++;

        // STATS once per second during capture.
        if (after_read - last_stats_us >= 1000000)
        {
            int64_t avg_period = periods_in_window > 0 ? sum_frame_period_us / periods_in_window : 0;
            int64_t avg_read = reads_in_window > 0 ? sum_sensor_read_us / reads_in_window : 0;
            size_t free_heap = esp_get_free_heap_size();
            size_t min_free_heap = esp_get_minimum_free_heap_size();
            UBaseType_t stack_hwm_words = uxTaskGetStackHighWaterMark(NULL);
            unsigned stack_hwm_bytes = (unsigned)stack_hwm_words * 4U;

            printf("STATS,%lld,%u,%u,%u,%lld,%lld,%lld\n",
                   after_read, (unsigned)free_heap, (unsigned)min_free_heap,
                   stack_hwm_bytes, avg_period, max_frame_period_us, avg_read);

            sum_frame_period_us = 0;
            max_frame_period_us = 0;
            sum_sensor_read_us = 0;
            periods_in_window = 0;
            reads_in_window = 0;
            last_stats_us = after_read;
        }

        // End the burst when the doorway has been clear long enough, or we hit
        // the safety cap (e.g. furniture left in the FOV). Flush any open track,
        // re-arm the wake tier, and go back to waiting.
        if (empty_streak >= CAPTURE_IDLE_FRAMES || capture_frames >= CAPTURE_MAX_FRAMES)
        {
            crossing_event_t fev = tracker_flush(&tk);
            if (fev.valid) { emit_event(&fev, esp_timer_get_time()); events_in_burst++; }
            if (sensor_arm_autonomous(&Dev, &motion_config, thresholds))
            {
                ESP_LOGE(TAG, "Failed to re-arm autonomous mode");
            }
            xQueueReset(vl53l5cx_queue);
            // DIAGNOSTIC: which capture stage is failing?
            //   frames==0          -> continuous didn't restart (sensor config)
            //   frames>0, blob<4   -> no person blob (background/data problem)
            //   frames>0, blob>=4, events==0 -> track too short / displacement
            //                                   too small (capture-start latency)
            ESP_LOGI(TAG, "Burst done: %d frames, peak blob %d cells, %d events — armed.",
                     capture_frames, max_blob, events_in_burst);
            state = STATE_ARMED;
        }
    }
}
