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
#include <stdint.h>                               // UINT32_MAX — instrumentation min-signal scan
#include <string.h>                               // memset() — zero the thresholds array
#include "freertos/FreeRTOS.h"                    // Core FreeRTOS types (tick counts, etc.)
#include "freertos/task.h"                        // vTaskDelay() — sleep without blocking other tasks
#include "freertos/queue.h"                       // FreeRTOS queue — the ISR->task hand-off
#include "esp_log.h"                              // ESP_LOGI/E — nicely formatted log output over serial
#include "esp_err.h"                              // ESP_ERROR_CHECK — aborts the program on a hardware error
#include "esp_timer.h"                            // esp_timer_get_time() — microsecond-resolution uptime for timestamps
#include "esp_system.h"                           // esp_chip_info() — get info about the ESP32 chip we're running on
#include "esp_sleep.h"                            // esp_light_sleep_start() + GPIO wake source (Phase 2 power)
#include "driver/i2c_master.h"                    // The new ESP-IDF v5 I2C driver (master-mode API)
#include "vl53l5cx_api.h"                         // ST's official driver for the sensor
#include "vl53l5cx_plugin_motion_indicator.h"     // Motion indicator plugin (autonomous wake)
#include "vl53l5cx_plugin_detection_thresholds.h" // Detection thresholds plugin (autonomous wake)
#include "driver/uart.h"                          // For UART configuration
#include "driver/gpio.h"                          // GPIO read/config — for the sensor INT pin
#include "detection/tracker.h"                    // Phase 2 detector port (pure C; pulls in background.h)
#include "storage/evbuf.h"                        // Phase 2 store-and-forward buffer (NVS-backed)
#include "net/net.h"                              // Phase 3 WiFi + MQTT client

// "Tag" used by the logging system. Every ESP_LOGI/ESP_LOGE line gets
// prefixed with this so you can filter logs by subsystem in the serial monitor.
static const char *TAG = "vl53l5cx";

// Per-node identity. Hardcoded for now; becomes MQTT-configurable in Phase 3.
// Flows into the buffered event / MQTT payload so the backend can attribute a
// crossing to a doorway.
#define NODE_ID "doorway-node-01"

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

// ---- power posture ----------------------------------------------------------
// CONFLICT (Phase 3): esp_light_sleep_start() powers the WiFi radio down, which
// drops the MQTT session every time we arm -- no heartbeats, no LWT semantics,
// no remote config while armed. Phase 3 needs a LIVE connection, so light sleep
// is compiled out (0) and ARMED blocks on the ISR queue instead, waking once a
// second to service MQTT. WiFi modem-sleep (set in net.c) still naps the radio
// between AP beacons. Reconciling real CPU sleep with a live connection
// (auto light sleep / tickless idle, or wake-then-connect) is the Phase 6
// power deep-dive; set to 1 to get the Phase 2 sleep behavior back.
#define POWER_LIGHT_SLEEP 0

// ---- two-tier mode parameters ---------------------------------------------
#define VL53L5CX_THRESHOLD_EVENT 1 // token the ISR pushes into the queue
#define MOTION_THRESHOLD 60        // motion-indicator level that counts as movement
                                   //   (ST's example value). Lower = wakes EARLIER
                                   //   (person at FOV edge, not center) = less of the
                                   //   crossing clipped before CAPTURING starts. Also
                                   //   the floor for the wake-validation gate in ARMED.
#define CAPTURE_FREQ_HZ 10         // continuous ranging rate while capturing a crossing
#define ARMED_FREQ_HZ 10           // autonomous ranging rate while waiting for motion
#define ARMED_INTEGRATION_MS 10    // integration time in autonomous mode
#define CAPTURE_IDLE_FRAMES 30     // re-arm after this many consecutive clear frames (~3s @10Hz)
#define CAPTURE_MAX_FRAMES 300     // safety cap on one capture burst (~30s @10Hz)

// ---- status/signal instrumentation (detection-review.md item E1) -----------
// Data-gathering mode to decide the status-255 semantics question (review A1)
// and the dark-clothing / signal-floor question (review D4) from real captures
// instead of argument. Two outputs on the serial line:
//   SIG,<t_us>,<min_kcps>,<min_cell>,<n_status255>   one per processed frame:
//       weakest per-SPAD return signal among cells that DID see a target
//       (kcps, fixed-point /2048), which cell it was, and how many cells
//       reported status 255 ("no target") this frame.
//   STATHIST,<label>,<status>,<frames>,<c0..c63>     dumped per phase (label
//       "calib" after bootstrap, "burst" after each capture burst), one line
//       per status bucket {5,9,10,255,other}: per-cell hit counts.
// Set to 0 to compile all of it out once the captures are taken.
#define INSTRUMENT_STATUS 1

#if INSTRUMENT_STATUS
#define N_STAT_BUCKETS 5 // statuses 5, 9, 10, 255, other
static const char *STAT_BUCKET_NAME[N_STAT_BUCKETS] = {"5", "9", "10", "255", "other"};
static uint16_t stat_hist[N_STAT_BUCKETS][64]; // u16: bursts cap at 300 frames
static uint32_t stat_hist_frames = 0;

static int stat_bucket(uint8_t s)
{
    switch (s)
    {
    case 5:   return 0;
    case 9:   return 1;
    case 10:  return 2;
    case 255: return 3;
    default:  return 4;
    }
}

// Accumulate one frame into the histogram and emit its SIG line.
static void instr_add_frame(const VL53L5CX_ResultsData *r, int64_t t_us)
{
    uint32_t min_sig = UINT32_MAX;
    int min_cell = -1;
    int n255 = 0;
    for (int i = 0; i < 64; i++)
    {
        uint8_t s = r->target_status[i];
        stat_hist[stat_bucket(s)][i]++;
        if (s == 255)
        {
            n255++; // no target: signal_per_spad is meaningless here
            continue;
        }
        if (r->signal_per_spad[i] < min_sig)
        {
            min_sig = r->signal_per_spad[i];
            min_cell = i;
        }
    }
    stat_hist_frames++;
    // signal_per_spad is fixed-point kcps/SPAD; /2048 gives integer kcps
    // (same scaling ST's examples print).
    printf("SIG,%lld,%lu,%d,%d\n", t_us,
           min_cell >= 0 ? (unsigned long)(min_sig / 2048u) : 0ul, min_cell, n255);
}

// Dump and reset the per-cell histogram (called at phase boundaries).
static void instr_dump(const char *label)
{
    for (int b = 0; b < N_STAT_BUCKETS; b++)
    {
        printf("STATHIST,%s,%s,%lu", label, STAT_BUCKET_NAME[b],
               (unsigned long)stat_hist_frames);
        for (int i = 0; i < 64; i++)
            printf(",%u", stat_hist[b][i]);
        printf("\n");
    }
    memset(stat_hist, 0, sizeof(stat_hist));
    stat_hist_frames = 0;
}
#endif // INSTRUMENT_STATUS

// Node operating tiers (see file header).
typedef enum
{
    STATE_CALIBRATING,
    STATE_ARMED,
    STATE_CAPTURING
} node_state_t;

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
    s |= vl53l5cx_motion_indicator_set_distance_motion(Dev, mc, 400, 1500);
    s |= vl53l5cx_set_ranging_mode(Dev, VL53L5CX_RANGING_MODE_AUTONOMOUS);
    s |= vl53l5cx_set_ranging_frequency_hz(Dev, ARMED_FREQ_HZ);
    s |= vl53l5cx_set_integration_time_ms(Dev, ARMED_INTEGRATION_MS);
    s |= vl53l5cx_set_detection_thresholds(Dev, thr);
    s |= vl53l5cx_set_detection_thresholds_enable(Dev, 1);
    s |= vl53l5cx_start_ranging(Dev);
    return s;
}

// ---- wake checkers: distance-window OR'd-across-the-grid with motion -------
// (detection-review.md item E3 / C3+D1.) The motion indicator needs 1-2
// autonomous frames of frame-to-frame variation before it trips INT — that
// accumulation is the biggest chunk of the wake latency clipping "out"
// crossings. A DISTANCE_MM checker with IN_WINDOW fires the FIRST frame any
// zone sees a return at person height: no accumulation at all.
//
// CONSTRAINT: the sensor takes at most 64 checkers total (one per zone at 8x8)
// and the AND/OR combiners are 4x4-only — so we cannot put BOTH checkers on
// every zone. Instead the grid is split checkerboard-style: half the zones get
// the distance checker, half keep motion. A person's blob spans 20+ cells at
// peak (Phase 1), so either checker type sees them — the OR happens spatially.
// Conveniently, hot pixels 1 and 5 land on the motion side of this parity.
#define WAKE_DIST_MIN_MM 500  // person-height return window (review C3): below
#define WAKE_DIST_MAX_MM 1900 //   = too close (noise), above = floor/background
#define WAKE_BG_MARGIN_MM 100 // keep-out margin around the window vs calibrated bg

// (Re)build the 64-checker array before each arm. Motion zones get the current
// trip level (MQTT-configurable, review's set_motion_threshold role). Distance
// zones get the IN_WINDOW checker — UNLESS that zone's calibrated background
// already sits in/near the window (doorframe edge, furniture): a static return
// inside the window would assert INT permanently and the node would never
// idle, so those zones fall back to motion. dist_zone[] records the final
// assignment for the wake-validation gate. Rebuilt at every arm, so it tracks
// the EMA-drifting background for free.
static void build_wake_thresholds(VL53L5CX_DetectionThresholds *thr, int motion_level,
                                  const background_model_t *bg, bool dist_zone[64])
{
    memset(thr, 0, VL53L5CX_NB_THRESHOLDS * sizeof(*thr));
    for (int i = 0; i < 64; i++)
    {
        int row = i / 8, col = i % 8;
        bool want_dist = ((row + col) % 2) == 0;
        if (want_dist)
        {
            if (!bg->calibrated[i])
                want_dist = false; // unknown background: motion is the safe checker
            else if (bg->bg[i] > (float)(WAKE_DIST_MIN_MM - WAKE_BG_MARGIN_MM) &&
                     bg->bg[i] < (float)(WAKE_DIST_MAX_MM + WAKE_BG_MARGIN_MM))
                want_dist = false; // static scene inside the window: would INT-storm
        }
        thr[i].zone_num = i;
        thr[i].mathematic_operation = VL53L5CX_OPERATION_NONE; // combiners are 4x4-only
        if (want_dist)
        {
            thr[i].measurement = VL53L5CX_DISTANCE_MM;
            thr[i].type = VL53L5CX_IN_WINDOW;
            thr[i].param_low_thresh = WAKE_DIST_MIN_MM;
            thr[i].param_high_thresh = WAKE_DIST_MAX_MM;
        }
        else
        {
            thr[i].measurement = VL53L5CX_MOTION_INDICATOR;
            thr[i].type = VL53L5CX_GREATER_THAN_MAX_CHECKER;
            // GREATER_THAN_MAX: low/high set equal to avoid ambiguity over
            // which field the checker reads (ST's example does the same).
            thr[i].param_low_thresh = motion_level;
            thr[i].param_high_thresh = motion_level;
        }
        dist_zone[i] = want_dist;
    }
    thr[63].zone_num |= VL53L5CX_LAST_THRESHOLD; // mark the final checker
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

// Persist a crossing to the store-and-forward buffer so it survives a reboot /
// connectivity outage until the Phase 3 MQTT client drains it. Maps the
// detector's event onto the compact on-flash record. t_us is the esp_timer
// timestamp at detection (boot-relative until Phase 4 NTP stamps wall-clock).
static void buffer_event(const crossing_event_t *e, int64_t t_us)
{
    evbuf_record_t r = {
        .t_us = t_us,
        .direction = (e->direction == DIR_IN) ? 1 : 0,
        .net = e->net,
        .confidence = e->confidence,
        .peak_blob = (uint16_t)e->peak_blob,
    };
    evbuf_enqueue(&r);
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

    fflush(stdout);                                   // Flush any buffered output before we reconfigure UART
    vTaskDelay(pdMS_TO_TICKS(20));                     // Let pending UART output drain (no driver installed, so
                                                       //   uart_wait_tx_done can't be used — a short delay suffices)
    uart_set_baudrate(UART_NUM_0, 921600);            // 921600 baud keeps up with 10+ FPS of 8x8 frames

    // Light sleep (ARMED tier) must flush the console UART FIFO before halting,
    // or the last log line is cut off. The console port has no driver, so this
    // sleep-time flush mode is the supported way to do it (not uart_wait_tx_done).
    esp_sleep_set_console_uart_handling_mode(ESP_SLEEP_ALWAYS_FLUSH_UART);

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

    // Detection thresholds: trip the INT when a motion zone's indicator exceeds
    // the trip level OR a distance zone sees a person-height return. The array
    // is (re)built by build_wake_thresholds() before every arm — it needs the
    // calibrated background, which doesn't exist yet here. static => kept off
    // app_main's stack. dist_zone[] mirrors which zones carry the distance
    // checker, for the wake-validation gate.
    static VL53L5CX_DetectionThresholds thresholds[VL53L5CX_NB_THRESHOLDS];
    static bool dist_zone[64];
    memset(thresholds, 0, sizeof(thresholds));
    memset(dist_zone, 0, sizeof(dist_zone));

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

    // Store-and-forward buffer. Persisted in NVS, so any crossings left unsent
    // from a previous run (power loss / WiFi outage) are still here on boot and
    // will be drained once the Phase 3 MQTT client exists.
    evbuf_init();
    ESP_LOGI(TAG, "Node %s — %u buffered event(s) pending forward.",
             NODE_ID, (unsigned)evbuf_count());

    // Phase 3: bring up WiFi + MQTT in the background (must follow evbuf_init —
    // WiFi needs NVS). Anything already buffered (including events from before
    // this boot) drains via net_drain_step() once the broker session is up.
    net_init(NODE_ID);

    // Current motion-threshold trip level; MQTT config can change it, applied
    // at the next (re-)arm.
    int motion_threshold = MOTION_THRESHOLD;

    node_state_t state = STATE_CALIBRATING;
    int empty_streak = 0;    // consecutive clear frames during a capture burst
    int capture_frames = 0;  // length of the current burst (safety bound)
    int max_blob = 0;        // diagnostic: peak largest-blob size seen this burst
    int events_in_burst = 0; // diagnostic: events emitted this burst

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
#if POWER_LIGHT_SLEEP
            // LIGHT SLEEP until the sensor's INT pulls the line LOW. The ESP32
            // halts its CPU (drops from ~mA to hundreds of uA) while the sensor
            // keeps ranging autonomously on its own. Light, NOT deep: deep sleep
            // wipes RAM, which would force re-uploading the sensor firmware
            // (~80 KB over I2C, 2-3 s) and re-seeding the background every wake.
            //
            // GPIO light-sleep wake is LEVEL-triggered only (edge isn't supported
            // in light sleep), and INT is active-low, so we wake on LOW. The
            // sensor holds INT low only until we read a frame; the ACK below
            // clears it back high, so the next sleep won't trip instantly.
            gpio_wakeup_enable(SENSOR_INT_GPIO, GPIO_INTR_LOW_LEVEL);
            esp_sleep_enable_gpio_wakeup();

            // Drain serial so the sleep doesn't truncate a half-printed line.
            // The FIFO itself is flushed by esp_light_sleep_start() because we
            // set ESP_SLEEP_ALWAYS_FLUSH_UART once at startup; fflush() just
            // pushes libc's buffer into that FIFO first. (Do NOT use
            // uart_wait_tx_done here -- it needs a uart_driver_install'd port,
            // which the console UART is not, and errors every cycle.)
            fflush(stdout);

            esp_light_sleep_start(); // <-- CPU stops here until INT goes LOW

            // Awake. Drop the level wake source while we service the sensor. The
            // edge ISR may push a token while INT is still low here; harmless --
            // the xQueueReset() below clears it.
            gpio_wakeup_disable(SENSOR_INT_GPIO);
#else
            // Phase 3 posture: BLOCK on the ISR queue instead of sleeping, so
            // WiFi/MQTT stays alive while armed. The 1 s timeout is the MQTT
            // service tick: drain one buffered event toward the broker, send
            // the heartbeat if due, and pick up any pushed config. The CPU
            // still idles in the blocked wait — just no radio-killing sleep.
            uint32_t evt;
            if (xQueueReceive(vl53l5cx_queue, &evt, pdMS_TO_TICKS(1000)) == pdFALSE)
            {
                net_drain_step();
                net_heartbeat_step();
                int mt = net_motion_threshold(MOTION_THRESHOLD);
                if (mt != motion_threshold)
                {
                    // Applied at the NEXT arm — build_wake_thresholds() reads
                    // this level when it rebuilds the checker array. Good
                    // enough: re-arm happens after every burst.
                    motion_threshold = mt;
                    ESP_LOGI(TAG, "motion_threshold -> %d (applies on next arm)", mt);
                }
                continue; // nothing moved; keep waiting
            }
#endif

            // ACK the wake at the sensor. ST's ULD requires check_data_ready +
            // get_ranging_data to clear the data-ready status that the INT line
            // mirrors; skipping it (the old code did) left the autonomous
            // interrupt asserted across the mode switch — half the cause of the
            // spurious second burst. INT guarantees a frame is coming, so poll
            // briefly for it rather than assuming it's already latched.
            isReady = 0;
            for (int tries = 0; tries < 5 && !isReady; tries++)
            {
                vl53l5cx_check_data_ready(&Dev, &isReady);
                if (!isReady)
                    vTaskDelay(pdMS_TO_TICKS(5));
            }
            if (isReady)
                vl53l5cx_get_ranging_data(&Dev, &Results);

            // VALIDATE the wake against the same motion the sensor's per-zone
            // checkers evaluate. A freshly re-armed motion indicator (re-init'd
            // every arm) plus residual motion from the person who just left can
            // trip INT once with no real crossing; requiring a live aggregate
            // above MOTION_THRESHOLD drops that false wake.
            //   NOTE: the 64 detection-threshold checkers are per-zone, but the
            //   readback motion[] array is hardware-capped at 32 aggregates
            //   (map_id[64] folds the 64 zones onto <=32). Scan nb_of_aggregates
            //   (<=32) — indexing motion[i] for i>=32 overruns the array.
            uint32_t peak_motion = 0;
            uint8_t n_aggr = Results.motion_indicator.nb_of_aggregates;
            if (n_aggr > 32)
                n_aggr = 32; // defensive: motion[] is only 32 wide
            for (uint8_t i = 0; i < n_aggr; i++)
                if (Results.motion_indicator.motion[i] > peak_motion)
                    peak_motion = Results.motion_indicator.motion[i];

            // Distance-checker evidence: how many distance zones see a return
            // inside the wake window right now? Statuses 5/9/10 only — 255
            // means "no target", its distance field is garbage (review A1).
            // Scanning only dist_zone[] zones keeps hot pixels (motion parity)
            // and bg-in-window zones out of the count by construction.
            int zones_in_window = 0;
            if (isReady)
            {
                for (int i = 0; i < 64; i++)
                {
                    if (!dist_zone[i])
                        continue;
                    uint8_t s = Results.target_status[i];
                    if (s != 5 && s != 9 && s != 10)
                        continue;
                    int d = Results.distance_mm[i];
                    if (d >= WAKE_DIST_MIN_MM && d <= WAKE_DIST_MAX_MM)
                        zones_in_window++;
                }
            }

            xQueueReset(vl53l5cx_queue); // drop arm-transient / queued INT pulses

            if (!isReady || (peak_motion < (uint32_t)motion_threshold && zones_in_window == 0))
            {
                // False wake: no aggregate moving AND nothing at person height.
                // Stay ARMED — the sensor is still ranging autonomously, INT is
                // now cleared, and the next real trigger will re-fire it.
                continue;
            }

            // BACKFILL (review C2/D2): the ACK frame we just read contains the
            // person's position AT WAKE — feed it to the detector instead of
            // throwing it away. The track starts one frame earlier, and the
            // tracker's grace frames then bridge the mode-switch gap below.
            // Directly attacks the directional clipping bug: the "out" path
            // enters the FOV corner the grid sees last, so every recovered
            // frame extends that direction's observable arc.
            int wake_blob = 0;
            {
                int64_t wake_us = esp_timer_get_time();
                int dist_w[64], status_w[64];
                for (int i = 0; i < 64; i++)
                {
                    dist_w[i] = Results.distance_mm[i];
                    status_w[i] = Results.target_status[i];
                }
#if INSTRUMENT_STATUS
                // Wake frames use ARMED_INTEGRATION_MS (10ms) — possibly
                // noisier than continuous frames. Instrumenting them answers
                // the review's quality caveat empirically.
                instr_add_frame(&Results, wake_us);
#endif
                bool occ_w[64];
                float dev_w[64];
                bg_process(&bg, dist_w, status_w, occ_w, dev_w);
                int cells_w[64];
                wake_blob = det_largest_blob(occ_w, cells_w);
                crossing_event_t wev = tracker_update(&tk, cells_w, wake_blob, dev_w, wake_us);
                if (wev.valid)
                {
                    emit_event(&wev, wake_us);
                    buffer_event(&wev, wake_us);
                }
            }

            if (sensor_start_continuous(&Dev))
            {
                ESP_LOGE(TAG, "Failed to switch to continuous on wake");
            }
            state = STATE_CAPTURING;
            empty_streak = 0;
            capture_frames = 0;
            max_blob = wake_blob; // the wake frame counts toward burst diagnostics
            events_in_burst = 0;
            last_frame_us = 0;
            last_stats_us = esp_timer_get_time();
            // Logs BOTH wake signals so the two paths' latencies can be
            // compared empirically (review E3): a distance-led wake shows
            // zones>0 with motion still under the threshold.
            ESP_LOGI(TAG, "Wake (motion peak %lu/thr %d, %d dist zones in window) — capturing.",
                     (unsigned long)peak_motion, motion_threshold, zones_in_window);
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
        for (int i = 0; i < 64; i++)
            printf(",%d", Results.distance_mm[i]);
        for (int i = 0; i < 64; i++)
            printf(",%d", Results.target_status[i]);
        printf("\n");

#if INSTRUMENT_STATUS
        instr_add_frame(&Results, after_read);
#endif

        // -------------------- CALIBRATING -----------------------------------
        if (state == STATE_CALIBRATING)
        {
            bg_bootstrap_add(&bg, dist_i, status_i);
            boot_frames++;
            if (boot_frames >= BG_BOOTSTRAP_FRAMES)
            {
                bg_bootstrap_finalize(&bg);
#if INSTRUMENT_STATUS
                instr_dump("calib"); // empty-doorway reference histogram
#endif
                ESP_LOGI(TAG, "Background calibrated (%d frames). Arming.", boot_frames);
                build_wake_thresholds(thresholds, motion_threshold, &bg, dist_zone);
                int n_dist = 0;
                for (int i = 0; i < 64; i++)
                    if (dist_zone[i])
                        n_dist++;
                ESP_LOGI(TAG, "Wake checkers: %d distance-window, %d motion.", n_dist, 64 - n_dist);
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
            if (period > max_frame_period_us)
                max_frame_period_us = period;
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
            buffer_event(&ev, after_read);
            events_in_burst++;
        }

        // Is the doorway clear this frame (no qualifying blob)?
        if (blob_n < TRK_MIN_BLOB_CELLS)
            empty_streak++;
        else
            empty_streak = 0;
        if (blob_n > max_blob)
            max_blob = blob_n;
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
            if (fev.valid)
            {
                int64_t flush_us = esp_timer_get_time();
                emit_event(&fev, flush_us);
                buffer_event(&fev, flush_us);
                events_in_burst++;
            }
            build_wake_thresholds(thresholds, motion_threshold, &bg, dist_zone);
            if (sensor_arm_autonomous(&Dev, &motion_config, thresholds))
            {
                ESP_LOGE(TAG, "Failed to re-arm autonomous mode");
            }
            xQueueReset(vl53l5cx_queue);
#if INSTRUMENT_STATUS
            instr_dump("burst"); // crossing histogram (incl. the wake frame)
#endif
            // DIAGNOSTIC: which capture stage is failing?
            //   frames==0          -> continuous didn't restart (sensor config)
            //   frames>0, blob<4   -> no person blob (background/data problem)
            //   frames>0, blob>=4, events==0 -> track too short / displacement
            //                                   too small (capture-start latency)
            ESP_LOGI(TAG, "Burst done: %d frames, peak blob %d cells, %d events — armed.",
                     capture_frames, max_blob, events_in_burst);
            // REJECT DIAGNOSTIC: when a fat blob emits no event, this says which
            // gate killed the track and the actual net displacement (cells).
            //   net_too_small -> capture-start latency clipped the crossing
            //   too_few_frames -> track shorter than TRK_MIN_TRACK_FRAMES
            if (events_in_burst == 0 && max_blob >= TRK_MIN_BLOB_CELLS)
            {
                const char *why =
                    (tk.last_reason == TRK_CLOSE_NET_TOO_SMALL)    ? "net_too_small"
                    : (tk.last_reason == TRK_CLOSE_TOO_FEW_FRAMES) ? "too_few_frames"
                    : (tk.last_reason == TRK_CLOSE_NOT_ACTIVE)     ? "not_active"
                    : (tk.last_reason == TRK_CLOSE_NONE)           ? "never_closed"
                                                                   : "emitted?";
                ESP_LOGW(TAG, "  reject: %s (net=%.2f cells, centroids=%d, need net>=%.1f & frames>=%d)",
                         why, tk.last_net, tk.last_n_centroids,
                         (float)TRK_MIN_NET_DISPLACEMENT, TRK_MIN_TRACK_FRAMES);
            }
            state = STATE_ARMED;
        }
    }
}
