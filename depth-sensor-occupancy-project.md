# DIY Depth Sensor Occupancy & Flow Monitoring System — Master Project Document

## How to Use This Document

You are guiding a developer through building a DIY depth-sensor-based occupancy and flow monitoring
system for their home. This document is the single source of truth for all project context,
decisions, and technical direction. The developer will work through this project **one phase at a
time**.

### For the Developer

1. **This file lives in the repo root** and is the project's single source of truth. Point Claude
   Code at it when starting a phase (e.g. `@depth-sensor-occupancy-project.md`).
2. **Work one phase at a time.** Tell Claude Code which phase you're starting; design,
   implementation, and debugging all happen in the same place.
3. **After completing each phase,** update the Decisions Log below with key decisions and outcomes.
   Keep entries tight (decision + why + any gotcha + what's still open); skip detail the code already
   records. If a decision changes later phases (e.g. switching from ESP32 to Pi Zero), edit the
   affected sections directly.

### For Claude Code

- **Always check the Decisions Log before responding.** Entries there override anything in later
  phases that contradicts them. If a phase says "ESP32" but the Decisions Log says "switched to Pi
  Zero in Phase 1," follow the Decisions Log.
- **Learning is the priority.** Explain concepts thoroughly. Don't skip steps. When there's a
  tradeoff between "easier but you learn less" and "harder but you understand the full picture,"
  lean toward the latter — but flag the tradeoff so the developer can decide.
- **Treat the Project Overview and Decisions Log as essential context** for whichever phase is being
  worked on.
- **The developer is strong at programming but new to hardware.** Provide step-by-step wiring
  instructions and explain electronics concepts as you go.
- **The developer builds and flashes the firmware themselves.** Don't run `pio run` / upload — make
  the code changes and let them build, flash, and report back.

---

## Decisions Log

_Update this section after completing each phase. These entries are authoritative — they override
any contradicting details in the phase descriptions below._

- **Pre-Phase 1** Framework: ESP-IDF (not Arduino)

  Embedded systems is a stated learning goal. Will use PlatformIO with framework = espidf on the Freenove ESP32-WROOM boards. Project doc references to "Arduino sketch" should be mentally translated to ESP-IDF equivalents (app*main(), FreeRTOS tasks, native esp*\* APIs). 2026-05-05

- **Pre-Phase 1** Sensor: VL53L5CX confirmed

  8×8 ToF imager. 63° FOV well-matched to doorway geometry at ~2m mounting height. ESP-IDF driver landscape solid via rjrp44/vl53l5cx on Espressif Component Registry (built on ST's official ULD). VL53L8CX is fallback if Phase 1 testing shows 8×8 resolution insufficient. 2026-05-05

- **Pre-Phase 1** Sensor vendor: ST VL53L5CX-SATEL

  ST's own eval board, ordered from DigiKey Canada (~$25 CAD). Chosen over SparkFun ($32.50) and Pololu ($19.95) for: (a) it's the reference board the rjrp44 ESP-IDF library targets, (b) cheapest legit name-brand option, (c) closer to professional embedded workflow which aligns with ESP-IDF choice. Ordering one for Phase 1; second unit at Phase 4. 2026-05-05

- **Phase 1** Sensor wiring verified

  SATEL on breadboard with ESP32. Connections: GND→GND, 3V3→IOVDD, 3V3→PWREN, GPIO 21→SDA, GPIO 22→SCL. AVDD reads 3.3V from internal regulator once PWREN is high — no external AVDD wire needed. I²C scanner detects sensor at 0x29 at 100kHz. 2026-05-13

- **Phase 1** VL53L5CX basic ranging working on ESP32

  First sensor node (`doorway-node-01`) streaming live 8×8 depth grids at 10 Hz over USB serial. Stack: PlatformIO + ESP-IDF v5.x, `rjrp44/vl53l5cx` v4.0.1 via ESP-IDF component manager (NOT PlatformIO `lib_deps` — that doesn't work with the Espressif Component Registry). Key gotchas hit and resolved: (a) library v4.x dropped the legacy `driver/i2c.h` driver in favor of `driver/i2c_master.h` — SDA/SCL/freq are configured at **runtime** in `i2c_master_bus_config_t`, not via menuconfig (older docs/guides claiming Kconfig pin options are wrong for v4.x); (b) `Dev.platform.handle` must be populated via `i2c_new_master_bus()` + `i2c_master_bus_add_device()` before any API call — setting `Dev.platform.address` alone causes a `LoadProhibited` crash inside `i2c_master_multi_buffer_transmit` (uninit stack pattern `0xa5a5a5a5`); (c) device address passed to `i2c_master_bus_add_device` must be `VL53L5CX_DEFAULT_I2C_ADDRESS >> 1` (7-bit form, 0x29); (d) firmware upload during `vl53l5cx_init` (~84 KB over I2C, 2–3 s) overflows the default 3584-byte app_main stack — bumped `CONFIG_ESP_MAIN_TASK_STACK_SIZE` to 7168 via `sdkconfig.defaults` (committed; survives clean builds, unlike the generated `sdkconfig.esp32dev`). I2C running at 400 kHz on GPIO 21 (SDA) / GPIO 22 (SCL); 8×8 resolution at 10 Hz ranging frequency. 2026-05-13

- **Phase 1** Sensor adequacy and compute platform confirmed — proceed with VL53L5CX + ESP32

  VL53L5CX 8×8 confirmed SUFFICIENT for doorway detection across 4 capture sessions (baseline, normal
  walks, fast walks, stop-in-doorway; ~2,300 frames, 0 dropped):

  - Person signal is unambiguous: 900+mm deviation from background, 20–32 occupied cells at peak. No
    risk of person-vs-noise confusion.
  - Direction recoverable: fast walks gave 8/8 clean direction alternation; normal walks 6/8 (2
    failures were analysis artifacts — global centroid + uncorrected corner cells — not sensor
    limits).
  - ~15–20 frames/crossing (normal), ~8–12 (fast) at 10 Hz. Ample.
  - Stop-in-doorway clearly separable: 4–7s sustained occupancy plateau vs 1.5–3s walk.
  - Travel is DIAGONAL across the grid (sensor rotated vs walking path) — Phase 2 must use
    2D/principal-axis centroid, not single-axis.
  - Two-people-simultaneous NOT tested; deferred to real-world observation.

  ESP32 confirmed SUFFICIENT with large margin:

  - 9.96 Hz steady, 0 dropped frames across all sessions.
  - Free heap 298,612 B (~292 KB), perfectly flat — no leaks.
  - Stack HWM 7,392 B free. Sensor read ~35ms = 35% of 100ms frame budget; ~65ms/frame available for
    Phase 2/3 work. Could sustain 15 Hz if needed.

  Phase 2 algorithm requirements identified from the data:

  1. Continuous/per-session background recalibration (mount drift observed: cell (0,1) drifted 935mm
     between baseline and walks — stale baseline is unsafe).
  2. Hot-pixel masking (cell (0,5) fires constantly).
  3. Connected-component blob tracking (track largest blob; ignore persistent small artifacts) — not
     global centroid.
  4. 2D / principal-axis direction detection.

  Captured sessions archived as Phase 2 test fixtures: empty_baseline, walks_alternating_out_first,
  fast_walks_alternating_out_first, stop_in_doorway_out_first (+ matching stats files). 2026-06-07

- **Phase 2** Detection core complete — Python golden reference validated against all fixtures
  Detection algorithm designed and validated entirely in Python first (core/harness split:
  portable plain-Python cores destined for the C port, numpy-heavy harness laptop-only). Two modules.
  `background.py` — perception (which cells are occupied this frame):
  - Per-cell background via median + MAD over a 150-frame bootstrap. Median tolerates a person
    wandering through calibration; MAD gives each cell its own noise floor, so jittery edge cells
    self-assign a higher threshold.
  - Continuous gated EMA update (ALPHA=0.01): background learns only from frames deemed empty
    (per-frame gate) AND never folds a cell into bg while that cell is individually flagged hot
    (per-cell freeze). Per-frame gate stops absorbing a stationary person; per-cell freeze stops bg
    chasing a flickering edge cell. Solves Phase 1 finding #3 (mount drift) without the
    stop-in-doorway dissolve failure.
  - Hysteresis (K_OCCUPY=6, K_CLEAR=4): high bar to declare a cell occupied, low bar to clear, so
    boundary cells don't chatter.
  - Hot-pixel mask stored as DATA (HOT_PIXELS tuple), not a hardcoded `if`, so it generalizes to
    per-node auto-detection later.
  `tracker.py` — temporal logic (what a sequence of frames amounts to):
  - Detection by CONNECTIVITY, not magnitude: 8-connected largest-blob extraction (flood fill).
    Discards scattered noise — fixes Phase 1 finding #6 (global centroid failed because it averaged
    the person with stuck corner cells). 8-connectivity (incl. diagonals) because travel is diagonal
    (finding #5); 4-conn would fragment the blob. Gate on largest-blob size.
  - Deviation-weighted centroid: weights cells by depth deviation so the closest point
    (head/shoulders, highest SNR) dominates and fringe-cell flicker barely moves the track. Yields
    smooth sub-cell coordinates.
  - Track lifecycle state machine (IDLE → ACTIVE → CLOSE) with GRACE_FRAMES=3 to bridge brief gate
    dropouts (one dropped frame doesn't split a crossing) and MAX_TRACK_FRAMES=100 force-close
    (bounds the C buffer; catches stuck furniture in FOV).
  - Direction by projecting net displacement onto a per-node `in_axis` vector — NOT PCA. PCA's
    eigenvector sign flips arbitrarily between crossings and can't label in vs out; in/out is an
    irreducible physical fact about how the sensor sits relative to the two rooms, so it is
    calibrated once per install. in_axis = −mean(known-out displacements), normalized. For this
    node/mount: in_axis ≈ (−0.413, −0.911). MUST be recalibrated if the sensor is remounted.
  - Crossing-vs-loiter by NET displacement, not path length: walk-in-stop-walk-back-out nets ≈ 0 →
    no event (correct — occupancy didn't change); walk-through (even with a multi-second pause
    mid-crossing) nets large → event. This subsumes the stop-in-doorway problem (finding #7) — net
    displacement handles loiter-and-return as a natural zero, with no special-case plateau logic.
    Plateau duration / path straightness feeds confidence only.
  - Robust endpoints: start/end = mean of first/last k centroids, with k capped at frames//2 so the
    windows never overlap (overlapping windows understate net displacement on short tracks and were
    dropping fast crossings).
  - Key tuning outcome — MIN_BLOB_CELLS = 4 (lowered from 5): a fast, low-signal crossing skims the
  grid so quickly the connected blob crests ≥5 for only a single frame, though the crossing spans
  ~10 frames of sub-gate occupancy and the centroid sweeps cleanly across. A one-frame track yields
  no direction. The event was dying at the GATE — upstream of the displacement and track-length
  knobs — so sweeping those (incl. MIN_NET_DISPLACEMENT down to 1) had zero effect. Found by
  instrumenting actual frames, not tuning. Gate=4 recovers it; empty_baseline still emits 0 events
  at gate=4, so it stays safe against noise. LESSON: when a parameter sweep bottoms out with no
  effect, the problem is at a different stage than the knob — instrument, don't tune.
  - Validation (all fixtures): empty_baseline → 0 events; walks_alternating_out_first → 8/8 crossings,
  clean alternating direction; fast_walks → 8/8 alternating (held-out — in_axis derived from walks,
  scored on fast); stop_in_doorway → 4/4 alternating (2 round-trips, each crossing with a 4–6 s
  mid-pause; proves a pause neither splits the track nor suppresses the crossing). Meets the Phase 2
  success criterion (≥80% correct direction) with large margin.
  - Files: detector/{background,tracker}.py (portable cores — plain Python, no numpy, map 1:1 to C);
  detector/{replay_background,replay_tracker}.py (laptop harness — numpy/pandas OK). Fixtures in
  fixtures/. detector/ and fixtures/ live at the workspace root, siblings to doorway-node-01 (the
  firmware project), keeping the core/harness boundary physical.
  - Deferred debt: (a) bimodal edge cell (0,1) flickers between the doorframe and floor surfaces — no
  single background value represents it; currently harmless because the blob gate discards it as an
  isolated size-1 blob. Revisit as auto-detected unreliable-cell flagging during bootstrap when node
  2 arrives (its defect map will differ — generalizes finding #4). (b) Two-people-simultaneous still
  untested. 2026-06-07

- **Phase 2** VL53L5CX datasheet review — capabilities affecting later phases
  Reviewed the ST datasheet/ULD manual against current plans. Findings:
  - Autonomous low-power mode with programmable interrupt threshold (`vl53l5cx_set_ranging_mode`,
    `vl53l5cx_set_detection_thresholds`): the sensor can range autonomously and wake the host via its
    INT pin on a threshold crossing. Answers the open Phase 2 power question — the sensor CAN wake the
    host, so no continuous polling is required. Reshapes power design toward two tiers: a cheap
    sensor-level trigger wakes the ESP32 from deep sleep, then the host runs the full pipeline to
    confirm and get direction.
  - Multitarget per zone (up to 4 targets): a tool for the deferred two-people problem (Phase 4); the
    current firmware reads target 0 only.
  - Per-zone motion indicator: a possible complementary detection or wake signal; likely redundant
    given the clean depth signal — parked.
  - CORRECTION: 60 Hz is a 4×4-only figure; 8×8 caps at 15 Hz. Current 10 Hz / 8×8 is well within
    budget (Phase 1 headroom confirms ~15 Hz sustainable).
  - Cover-glass crosstalk compensation works beyond 60 cm and has a calibration routine
    (`vl53l5cx_calibrate_xtalk`) — relevant when the Phase 6 enclosure puts a window over the sensor.
  2026-06-07

- **Phase 2** Detector ported to C/ESP-IDF and integrated into firmware — native regression passes
  The validated Python cores were ported to pure C (zero ESP-IDF deps, so the SAME source compiles
  for the device and for a host-native test) and wired into the firmware frame loop. Done by Claude
  Code from a handoff prompt (`claude-code-handoff-cport.md`).
  - Files: `doorway-node-01/src/detection/{background,tracker}.{c,h}` (the port — pure C, `float`
    math for the single-precision ESP32 FPU, fixed-size buffers, no heap in the hot path). Native
    harness in `detector/native/`: `test_replay.c` driver, `Makefile` (standalone gcc — NOT a
    PlatformIO `native` env, which fights the espidf toolchain), `gen_golden.py` (golden generator)
    → `golden/*.txt`, and `run_regression.py` (tolerance gate: exact on direction/frames/peak/
    t_start, ±0.05 cells on dx/dy/net, ±0.05 on confidence).
  - Native regression PASSES all four fixtures: empty_baseline→0, walks→8 (out,in×4), fast_walks→8
    (out,in×4, held-out), stop_in_doorway→4 (out,in×2). Matches the Python golden within tolerance.
  - DECISION — `MIN_BLOB_CELLS = 4` in BOTH stages. During this session `background.py`'s bg-update
    gate was changed 5→4 (it had been 5 in the validated run; `tracker.py`'s track gate was already
    4). Rather than revert, kept 4 everywhere: re-ran the Python replay and confirmed gate=4 still
    yields 0/8/8/4 with clean direction. Ported as two SEPARATE constants (`BG_MIN_BLOB_CELLS`,
    `TRK_MIN_BLOB_CELLS`) — same value today but conceptually independent per-stage knobs; don't
    collapse into one `#define`.
  - DECISION — `in_axis` re-derived to ≈ (−0.456, −0.890) (was (−0.413, −0.911) at gate=5). The axis
    is gate-dependent because it's computed from the out-crossing displacement vectors. Hardcoded as
    `IN_AXIS_X/Y` in `tracker.h` and mirrored in `gen_golden.py`. STILL per-node/per-mount; MUST be
    recalibrated on remount; becomes MQTT-configurable in Phase 3.
  - Firmware integration: `main.c` copies each frame's 64 distances + 64 statuses into the detector,
    bootstraps `BG_BOOTSTRAP_FRAMES` empty frames on boot (logs "calibrating… ~15s / ready"), then
    emits `EVENT,<esp_timer_us>,<direction>,<net>,<confidence>,<peak_blob>` alongside the unchanged
    FRAME/STATS lines. New sources auto-picked-up by the existing `GLOB_RECURSE` in
    `src/CMakeLists.txt` (glob narrowed to `*.c`; added `INCLUDE_DIRS "." "detection"`). Builds clean
    via PlatformIO: RAM 16.5% (53,956 B), Flash 24.8% — the static background model (per-cell
    bootstrap buffer) fits with large margin; kept off the 7,168 B main-task stack via `static`.
  - On-device lifecycle differs from the offline harness by design: the device starts detection only
    AFTER the 150-frame bootstrap, whereas the harness reprocesses bootstrap frames. Harmless —
    bootstrap frames are empty (calibration), so no real crossings are lost.
  - NOT YET DONE deep sleep/power, store-and-forward, MQTT/networking, auto-detection of unreliable cells. 2026-06-07

- **Phase 2** Power management — two-tier wake/capture state machine (IN PROGRESS, not finished)
  Began the battery-operation work. The validated detector needs a DENSE continuous frame stream, so
  it cannot run on the sparse, motion-triggered frames the sensor's low-power mode produces. Resolved
  this with the two-tier design flagged in the datasheet-review entry: a cheap WAKE tier that only
  signals "something moved", and a CAPTURE tier that streams continuously and runs the full detector.
  - HARDWARE: sensor INT (open-drain, active-low) → ESP32 **GPIO 4**. Chosen because it's RTC-capable
    (needed for deep-sleep wake later), not a strapping pin, and free. Pull-up: started with the
    datasheet-specified external 47k to IOVDD, then switched to the ESP32's INTERNAL pull-up
    (~45k, `gpio_set_pull_mode(..., GPIO_PULLUP_ONLY)`) to simplify wiring. CAVEAT recorded in code:
    internal pull-ups power off in deep sleep — deep sleep will need `rtc_gpio_pullup_en()` on GPIO 4;
    light sleep retains it. INT is open-drain so it needs a pull-up at all (the sensor can only pull
    LOW); the sensor is powered via its own supply pins, INT is just a signal output.
  - EMPIRICAL FINDING: the INT pin only fires in **AUTONOMOUS mode with detection thresholds**, NOT in
    continuous ranging. So continuous capture uses I2C `check_data_ready` polling; the wake tier uses
    the INT.
  - FIRMWARE (`doorway-node-01/src/main.c`): three-state machine.
    * CALIBRATING — continuous 8x8 @10Hz; seed background over `BG_BOOTSTRAP_FRAMES` clear frames; arm.
    * ARMED — autonomous 8x8 @`ARMED_FREQ_HZ` (5) + motion indicator + per-zone detection thresholds
      (`GREATER_THAN_MAX`, level `MOTION_THRESHOLD`=44, distance window 500–1999 mm). Sensor pulses
      INT on motion; an ISR pushes a token to a FreeRTOS queue; the task blocks on `xQueueReceive`.
      **This is where ESP32 sleep will go (TODO(power) marker in the code).**
    * CAPTURING — switch to continuous @10Hz, run the detector (bg_process → largest blob → tracker),
      emit `EVENT` lines, until the doorway is clear for `CAPTURE_IDLE_FRAMES` (~2s) or a
      `CAPTURE_MAX_FRAMES` safety cap, then re-arm.
    * Mode switches via helpers `sensor_start_continuous()` / `sensor_arm_autonomous()` (stop →
      reconfigure → start; thresholds disabled in continuous, re-applied on every arm).
  - ISR→QUEUE pattern (and WHY): an ISR runs in interrupt context — no I2C, no printf, no blocking —
    so it can only signal. The queue carries that signal to the task, and (unlike a flag) lets the
    task BLOCK/sleep instead of busy-polling — which is the hook the coming light-sleep depends on.
  - Secondary fixes folded in from the example port: detection threshold low/high both = 44 (was 500,
    far too insensitive); sensor-config return codes now checked.
  - KNOWN OPEN ISSUE (where this session stopped): wake-then-capture inherently misses the
    *pre-trigger* part of the crossing, and the lost portion is DIRECTION-DEPENDENT. On hardware,
    walking "in" emits a correct EVENT, but walking "out" yields a solid blob (peak ~12 cells) whose
    track is rejected (too few frames / net displacement < 2 cells) → no event. Also occasional
    spurious re-wake bursts (peak blob 1, 0 events) right after a crossing. A burst diagnostic was
    added: `ESP_LOGI("Burst done: N frames, peak blob X, Y events")`. (Note: a deeper per-reject
    diagnostic — a `closed` flag on the event — was prototyped then reverted; the detector core is
    unchanged and still passes all four native regression fixtures.)
  - NEXT STEPS / DECISION PENDING:
    1. Shrink the wake→capture latency that clips "out": raise `ARMED_FREQ_HZ` toward 10–15 and trim
       the mode-switch overhead. If clipping persists, reconsider the architecture — keep the sensor
       ranging continuously and only light-sleep the ESP32 during idle (more sensor power, full
       crossings) vs. living with directional clipping.
    2. Implement ESP32 **LIGHT** sleep at the ARMED `TODO` (`esp_sleep_enable_gpio_wakeup` +
       `gpio_wakeup_enable(SENSOR_INT_GPIO, GPIO_INTR_LOW_LEVEL)` + `esp_light_sleep_start`). LIGHT,
       not deep: deep sleep loses RAM and would force the 2–3 s `vl53l5cx_init` firmware re-upload on
       every wake, missing the very crossing that woke it. Deep sleep is reserved for long idle
       windows (e.g. overnight) only.
    3. Measure actual power draw with a multimeter (the Phase 6 power-profiling task).
  2026-06-08

- **Phase 2** ESP32 light sleep at the ARMED tier (implementation done, validation open)
  ARMED now halts the CPU instead of busy-blocking a FreeRTOS queue (`gpio_wakeup_enable` LOW_LEVEL +
  `esp_sleep_enable_gpio_wakeup` + `esp_light_sleep_start`; the existing ACK + motion-validate gate runs
  unchanged after wake). Light NOT deep (confirmed via ESP-IDF docs): deep sleep wipes RAM + the GPIO 4
  internal pull-up → would re-upload sensor FW (~80 KB / 2-3 s) and re-seed the background every wake,
  missing the crossing that woke it; deep sleep stays reserved for long idle. GPIO light-sleep wake is
  level-only, INT is active-low → wake on LOW; safe because the sensor releases INT high once we read the
  frame, so the next sleep doesn't trip instantly. GOTCHA: `uart_wait_tx_done()` needs a
  `uart_driver_install`'d port; the console UART has none → `uart driver error` every cycle. Fix:
  `esp_sleep_set_console_uart_handling_mode(ESP_SLEEP_ALWAYS_FLUSH_UART)` once at boot lets
  `esp_light_sleep_start()` drain the TX FIFO itself; both `uart_wait_tx_done` calls removed.
  OPEN: (1) measure actual draw — no proof it engages yet (blocked on multimeter); (2) the sensor is now
  the power floor (autonomous ranging + I2C pull-ups draw while the ESP32 sleeps); (3) deep sleep for
  overnight is a separate task (needs `rtc_gpio_pullup_en` on GPIO 4 + FW re-upload + bg re-seed) — pursue
  only if the light-sleep floor proves too high. The directional clipping bug (prior entry) is unrelated
  and still open; detector core untouched, native regression still 0/8/8/4. 2026-06-09

- **Phase 2** Local buffering — NVS-backed store-and-forward ring (implemented; drain waits on Phase 3 MQTT)
  Phase 2 task 5: a crossing detected while connectivity is down (router/Pi/broker outage) is persisted
  and replayed instead of lost, else the backend occupancy count drifts permanently. WHY NVS over a
  LittleFS append-log: the buffer must survive the very events it covers (reboot/crash/power loss, all of
  which wipe RAM) so it must be flash-backed; NVS is least-machinery (already initialized, no
  partition-table edits, wear-levelled + power-loss safe). Trade: NVS is a key/value store faked into a
  FIFO and tighter on space — fine at our rate (a few crossings/min, short outages). DESIGN: fixed ring
  (`EVBUF_CAPACITY` 64), monotonic `head`/`tail` u32 in NVS (count = tail − head), event blob under key
  `e<seq % CAP>`, full → drop oldest, every mutation committed. `evbuf_record_t` is decoupled from the
  detector's `crossing_event_t` so the on-flash format maps 1:1 to the Phase 3 MQTT JSON; `seq` doubles as
  the Phase 4 dedup key. Files `src/storage/evbuf.{c,h}` (peek→publish→pop is the Phase 3 drain seam);
  `main.c` enqueues at both emit sites after the `EVENT` line. Native gcc regression unaffected (evbuf not
  in that build) — still 0/8/8/4. TEST w/o MQTT: walk → reset ESP32 (keeps NVS) → boot log shows N>0
  pending = cross-reboot persistence proven; `erase_flash` clears it. OPEN: (a) `t_us` is boot-relative
  µs, real ISO8601 needs Phase 4 NTP (stamp at publish); (b) `node_id` hardcoded, MQTT-configurable in
  Phase 3; (c) drain-to-MQTT loop is Phase 3. 2026-06-09


- **Phase 3** MQTT layer complete — broker, node client, store-and-forward drain, LWT, remote config (all success criteria tested)
  Mosquitto 2.x on the Pi 5 (`/etc/mosquitto/conf.d/local.conf`): `listener 1883 0.0.0.0` (default
  binds localhost only — ESP32 can't reach it), `allow_anonymous false`, per-client passwords
  (`doorway-node-01`, `backend`). GOTCHA: broker exits status 13 if `/etc/mosquitto/passwd` isn't
  readable by the `mosquitto` user — `chown mosquitto:` + `chmod 600`. Topics:
  `home/doorways/<node_id>/{events,status,config}` — events QoS1 non-retained (must arrive, must
  not replay to new subscribers), status+config retained (late subscriber instantly sees last
  state/config). LWT publishes retained `{"online":false}` to status; node overwrites on connect.
  - Firmware: new `src/net/` module. THREADING CONTRACT: esp-mqtt runs its own task; its handlers
    only set word-sized flags — ALL evbuf access stays in the main task via `net_drain_step()` /
    `net_heartbeat_step()` called from the ARMED 1s idle tick (evbuf is single-writer by design).
    Drain is peek → publish QoS1 → wait PUBACK → pop: an event leaves flash only after broker ack;
    PUBACK timeout (10s) re-peeks the same event, so worst case is a duplicate (seq = Phase 4 dedup
    key), never a loss. One event in flight at a time keeps ordering. Heartbeat 30s retained
    (uptime, heap, evbuf pending, RSSI). Remote config arrives on the retained config topic, parsed
    lazily in the main task (cJSON); `motion_threshold` applied at next re-arm — proven live, and
    survives node reboot via retention. Credentials in `src/net/secrets.h` (gitignored;
    `secrets.h.example` committed).
  - DECISION — light sleep compiled out for Phase 3 (`POWER_LIGHT_SLEEP=0` in main.c):
    `esp_light_sleep_start()` powers down the radio → MQTT session drops every ARMED entry → no
    heartbeats/LWT/config while armed. ARMED now blocks on the ISR queue with a 1s timeout that
    services MQTT. WiFi modem-sleep (`WIFI_PS_MIN_MODEM`) is the interim power posture. Sleep code
    kept behind the flag; reconciling CPU sleep with a live connection (auto light sleep/tickless
    idle vs wake-then-connect) is the Phase 6 power deep-dive.
  - TOOLCHAIN GOTCHA — PlatformIO now ships ESP-IDF 6.0, which evicted esp-mqtt and cJSON from
    core into the Component Registry: add `espressif/mqtt` + `espressif/cjson` to
    `src/idf_component.yml` (headers/APIs unchanged). Also: PlatformIO's `src` component is NOT
    `main`, so core components like esp_wifi need explicit `PRIV_REQUIRES` in `src/CMakeLists.txt`.
  - FLASH/RAM: WiFi+lwIP+MQTT pushed the binary to ~96% of the default 1MB app partition → custom
    `partitions.csv` (single ~1.9MB factory app; this module is 2MB flash, pinned via
    `CONFIG_ESPTOOLPY_FLASHSIZE_2MB`). OTA needs the app to fit twice — does NOT fit on 2MB at
    current size; revisit (trim or 4MB module) when OTA matters (Phase 6+). Main-task stack
    7168 → 12288 (`esp_wifi_init`/`start` overflowed it — same failure mode as the Phase 1
    vl53l5cx_init overflow). Remember: sdkconfig.defaults changes need `sdkconfig.esp32dev`
    deleted to take effect.
  - VERIFIED: events arrive at broker; broker stopped → 3 crossings buffered to NVS → broker
    restarted → drained in order, `pending` 3→0; USB pulled → LWT `online:false` within ~45s
    (1.5× keepalive 30s); retained config picked up after node reboot; anonymous publish refused.
    Old buffered events from pre-MQTT runs drained on first connect (their `t_us` is boot-relative
    per run — meaningless until Phase 4 NTP; backend should stamp arrival time meanwhile).
  - OPEN: (a) `t_us` boot-relative → Phase 4 NTP; (b) `in_axis` promised as MQTT config, not yet
    wired (only `motion_threshold` is); (c) TLS skipped (plain auth on trusted LAN) — optional
    learning exercise later; (d) registration is just the retained status message, no separate
    registry handshake; (e) heartbeat lacks battery level (no battery hardware yet).
  2026-06-09

- **Phase 3.5** Detection-logic review — `detection-review.md` (critique only, no code changed)
  Full critical review of background/tracker/wake-tier against the ULD source + UM2884. Headlines:
  status 255 = "no target detected" yet sits in VALID_STATUS (latent bug, possibly load-bearing
  for IR-absorbing targets — instrument before changing); 100-frame force-close can double-count
  a slow crossing (fix: O(1) endpoint tracking, no cap); open directional-clipping bug has two
  cheap untried fixes (feed the already-read wake frame to the detector; wake on DISTANCE
  IN_WINDOW checker OR'd with motion); persistent scene change (furniture) corrupts the detector
  with no recovery (needs absorption timeout). Prioritized plan in section E — items 1-3 don't
  touch the validated goldens. Fixes to be implemented from that doc as their own change-sets.
  2026-06-09

- **Phase 3.5** Review items E1–E3 implemented (instrumentation, wake-frame backfill, distance-window wake) — main.c only, detector core untouched, regression 0/8/8/4
  - E1 instrumentation (`INSTRUMENT_STATUS 1`, compile-out flag): per-frame `SIG,<t_us>,<min_kcps>,<min_cell>,<n255>` line (weakest signal_per_spad among target-seeing cells, /2048 = kcps) + per-cell status histogram (`STATHIST` lines, buckets 5/9/10/255/other) dumped after calibration ("calib" = empty reference) and each burst ("burst" = crossing). Wake frames are instrumented too — answers the review's "are 10ms-integration autonomous frames noisier?" caveat. This data gates items E4–E5 (status-255 semantics).
  - E2 backfill: the wake-ACK frame (previously discarded) now runs through bg_process→blob→tracker_update before the mode switch; its blob seeds the burst's `max_blob` diagnostic. Track starts 1 frame earlier + grace frames bridge the switch gap.
  - E3 distance wake — CONSTRAINT FOUND: sensor takes max 64 checkers and AND/OR combiners are 4×4-only, so motion-OR-distance per zone is impossible at 8×8. Solution: checkerboard split — even-parity zones get `DISTANCE_MM`/`IN_WINDOW`, odd keep motion (hot pixels 1, 5 land on motion parity for free); the OR happens spatially since a person spans 20+ cells. GOTCHA (hit on first flash): a FIXED window ceiling (1900mm) sat ~100mm under the floor (~2000mm) — floor noise dipped into the window → continuous INT storm. Fix: per-zone ceiling = calibrated bg − clearance, capped at 1900 (hardware bg-subtraction: "return significantly closer than this zone's floor"); window collapsed (bg too close) or uncalibrated → motion fallback. Checker array rebuilt from live bg at every arm. GOTCHA 2: bimodal edge cells defeat clearance math — zone 13 (1,5) flickers a doorframe surface ~320mm closer than its median bg with STATUS 5, so median+MAD never see it; it slipped a 300mm clearance and kept storming. Fix: clearance 300→400 + strike demotion — a distance zone whose wake yields an empty burst (no blob, no events) twice is demoted to a motion checker until reboot (self-healing per mount; generalizes the hand-coded HOT_PIXELS lesson — real crossings never strike because their burst has a blob). Wake-validation gate widened to motion≥thr OR ≥1 distance zone in window (distance wakes have no accumulated motion — old gate would have dropped them). Wake log now prints both signals for latency comparison.
  - CAPTURE RESULTS (capture/{empty,light,dark}.log, analyzed same night): clipping bug CLOSED — light 6/6 events clean alternation, dark 5/6 (one reject net 0.95 < 1.2); distance checker led most wakes (motion peak under 60 at wake). Strike demotion proven live: zones 13 + 4 struck out, node silent after. A1 VERDICT — status 255 is NOT person/dark-correlated: rate flat ~4% across empty/light/dark, concentrated in fixed FOV-corner cells (63, 62, 39 — geometry, not targets); the "255 is load-bearing for dark hair" fear is unsupported at this mount → tri-state fix (item E4) safe to proceed. A2 CONFIRMED — untrusted "other" statuses jump 1.2%→6.5% during crossings on person-path cells: mid-crossing status flicker is real, latch-hold justified; also explains dark's smaller blobs (8–17 vs 15–19 peak). D4 parked: min-over-grid signal_per_spad always 0 kcps (edge cells), and with 255 not dark-correlated there's no driver. SIDE FINDING: corner cells' bootstrap medians include 255-garbage samples (131/150 frames at cell 63) — E4's bootstrap exclusion will fix their bg for free. TUNING (user, on device): TRK_MIN_NET_DISPLACEMENT 1.5→1.2 (tracker.h), CAPTURE_IDLE_FRAMES 30→20; native regression REBUILT (stale-binary gotcha: run_regression.py doesn't compile — make first or PASS is against the old binary) and passes 0/8/8/4 at 1.2. NOTE: Python reference tracker.py still has MIN_NET_DISPLACEMENT=2 — C and Python constants now diverge; sync Python when E4's replay rerun happens anyway. 2026-06-09

- **Phase 4** Firmware tree made node-agnostic — `doorway-node-01/` → `doorway-node/`, per-node constants → committed `node_config_XX.h` selected by PlatformIO env
  One tree flashes every node (avoids fork drift at node 2). NODE_ID, NODE_NUM, IN_AXIS_X/Y, and
  the hot-pixel map (now `NODE_HOT_PIXELS`) moved out of main.c / tracker.h / background.c.
  Design: `src/node_config.h` is a committed dispatcher (`#include NODE_CONFIG_FILE`); each node's
  values live in a committed `src/node_config_XX.h`; `platformio.ini` has `[env:node01]` /
  `[env:node02]` each defining `NODE_CONFIG_FILE` + its upload port. Flash = `pio run -e nodeXX -t
  upload` — no file editing between nodes, all configs in git. Adding a node = new header + new
  env. The single gitignored `secrets.h` now covers all nodes: `MQTT_USER` = NODE_ID, password
  branches on NODE_NUM. GOTCHA: the native regression compiles the same cores, but the goldens
  depend on node 01's axis/hot-pixels — `detector/native/node_config.h` is a separate COMMITTED
  copy pinned to the fixture-capture values (Makefile `-I.` first; no NODE_CONFIG_FILE, so the
  dispatcher never runs on host). Never sync the two; if reference values change, regenerate
  goldens in the same change. No hot pixels on a node → `{-1}` sentinel (C rejects an empty
  initializer). Env rename means fresh generated `sdkconfig.node01/02` from `sdkconfig.defaults`
  (stale `sdkconfig.esp32dev` removed); `.pio` cache invalid after the dir rename — clean build
  expected. Native regression rebuilt and passes 0/8/8/4. Also per-node: `NODE_WAKE_DIST_MIN_MM`,
  `NODE_WAKE_DIST_CAP_MM`, `NODE_WAKE_BG_CLEARANCE_MM` (wake-window too-close / too-far + per-zone
  bg clearance — mount geometry; node 2's mount is much higher than node 1's). Cap must sit
  clearly under that mount's floor distance or floor noise storms the INT; on a higher mount a
  person's deviation is larger, so clearance can rise for extra storm margin. main.c aliases all
  three into the old WAKE_* names. Node 2 still needs: broker user (`mosquitto_passwd`), real
  password in secrets.h, in_axis calibration at mount, hot-pixel map, wake window (MIN/CAP/
  clearance) from measured floor distance. 2026-06-11

---

## Project Overview

### What We're Building

A distributed system of DIY depth sensor nodes mounted above doorways in a two-story house. Each
node detects when a person passes through the doorway, determines direction (in vs out), and
publishes entry/exit events over MQTT to a central backend. The backend aggregates these events into
a real-time occupancy model of the house, displayed on a 2D digital twin dashboard — a floor plan
view showing which rooms are occupied and how people flow through the space.

### Why This Project Exists

This is a **personal learning project** with no deadline. The developer wants hands-on experience
with:

- Hardware and electronics (relatively new, needs step-by-step guidance)
- Edge computing (processing sensor data on constrained devices)
- IoT (device management, MQTT, connectivity)
- Distributed systems (multi-node coordination, clock sync, deduplication)
- Digital twins (real-time virtual representation of a physical space)

**Learning is the priority over speed.** Explain concepts thoroughly. Don't skip steps or hand-wave
over complexity. When there's a choice between "easier but you learn less" and "harder but you
understand the full picture," lean toward the latter — but flag the tradeoff so the developer can
decide.

### Developer Profile

- **Programming:** Strong. Comfortable with Python, C++, Java, React. Has not worked with computer
  vision or point cloud data, but has done sensor data projects (e.g., using a Wii remote to control
  a PC mouse).
- **Hardware:** Beginner. Has a Raspberry Pi introduction electronics kit (one breadboard, jumper
  wires, capacitors). Needs step-by-step wiring instructions with diagrams or clear pin-by-pin
  descriptions. No multimeter yet — needs to buy one.
- **Electronics tools on hand:** 1 breadboard, jumper wires, capacitors, basic components from an
  intro kit.
- **Electronics tools to buy:** Multimeter ($15-20), soldering iron kit, additional breadboards, and
  components as specified per phase.

### Hardware Already Owned

| Item                      | Quantity | Notes                                                         |
| ------------------------- | -------- | ------------------------------------------------------------- |
| Raspberry Pi 5 (16GB RAM) | 1        | Will serve as the central broker/backend                      |
| ESP32 dev boards          | 2        | Will serve as edge sensor nodes (if capable) or be repurposed |
| Basic electronics kit     | 1        | Breadboard, jumper wires, capacitors                          |

### Key Architectural Decisions (Already Made)

1. **Depth sensors:** ToF (Time-of-Flight) sensor modules, wired up DIY-style. Not building IR
   projector + camera from scratch.
2. **Sensor nodes:** Start with ESP32s. If processing demands exceed ESP32 capabilities, buy
   Raspberry Pi Zeros as node compute boards. Budget is modest but flexible for sensors and small
   boards.
3. **Central hub:** Raspberry Pi 5 runs MQTT broker (Mosquitto), backend service, and dashboard.
4. **Communication:** MQTT over WiFi. Reliable WiFi coverage confirmed throughout the house.
5. **Power:** Battery operated for production use. Nodes need to run for days/weeks, so deep sleep
   modes and power management are critical design constraints.
6. **Scope:** Start with 2 doorways as proof of concept, expand later to bedroom, kitchen, front
   door, side door, etc.
7. **Dashboard:** 2D floor plan digital twin showing real-time occupancy. Must handle two floors.
   Built in React given developer experience.
8. **OS:** Raspberry Pi OS on the Pi 5 (can be changed if needed).
9. **Privacy:** Depth-only sensing means no identifiable imagery is ever captured. This is a
   feature, not a limitation.

### Shopping List (To Be Refined Per Phase)

This is a preliminary list. Each phase should confirm exactly what's needed before the developer
buys anything.

- **ToF sensor modules:** VL53L5CX (8x8 depth grid, I2C, ~$15-25 each) — need 2 minimum. _Note: 8x8
  may be too low resolution for reliable person detection. Phase 1 should evaluate this and
  recommend alternatives if needed (e.g., VL53L8CX for higher resolution, or multiple VL53L5CX units
  per doorway)._
- **Multimeter:** Any basic digital multimeter (~$15-20)
- **Soldering iron kit:** If not already owned (~$25-30)
- **Additional breadboards:** 1-2 more for prototyping
- **Jumper wires:** Ensure enough male-to-male, male-to-female, female-to-female
- **Battery power:** LiPo batteries + charging/management boards (TP4056 or similar). Size depends
  on power budget calculated in Phase 2.
- **Pi Zero W/2W:** Possibly 2, if ESP32 proves too constrained. Decision made in Phase 2.
- **Mounting hardware:** 3D-printed or simple enclosures for doorframe mounting (later phases)
- **MicroSD cards:** For any additional Pi boards

---

## Phase 1: Hardware Assembly & Sensor Validation (Single Node)

### Goal

Get a single ToF sensor wired to an ESP32, reading depth data, and displaying it on screen. Validate
whether the sensor resolution and range are sufficient to detect a person passing through a doorway.
Make the go/no-go decision on ESP32 vs Pi Zero for edge nodes.

### Learning Objectives

- I2C communication protocol (how the sensor talks to the microcontroller)
- Reading datasheets and understanding sensor specifications
- Basic circuit wiring and debugging with a multimeter
- Setting up a development environment for ESP32 (Arduino IDE or PlatformIO)

### What to Buy Before Starting This Phase

- 1x VL53L5CX ToF sensor breakout board (SparkFun and Adafruit both sell breakouts with headers)
- 1x digital multimeter
- Jumper wires (male-to-female for connecting sensor to ESP32)

### Tasks

1. **Environment setup:** Install Arduino IDE or PlatformIO. Set up ESP32 board support. Verify you
   can upload a basic blink sketch to the ESP32.

2. **Understand the sensor:** Read the VL53L5CX datasheet/product page. Understand: what is an 8x8
   depth grid? What range does it cover (max ~4 meters)? What is its field of view? What is I2C and
   how does it work at a basic level?

3. **Wiring:** Connect VL53L5CX to ESP32 via I2C. This means 4 wires:
   - VCC → 3.3V on ESP32
   - GND → GND on ESP32
   - SDA → a GPIO pin configured for I2C SDA (typically GPIO 21)
   - SCL → a GPIO pin configured for I2C SCL (typically GPIO 22)

   Provide a clear pin-by-pin wiring guide. Explain pull-up resistors if needed (many breakout
   boards include them).

4. **First reading:** Write a simple sketch/script that initializes the sensor over I2C, reads the
   8x8 depth grid, and prints it to serial monitor. Visualize it — even as a text grid of numbers
   updating in the terminal.

5. **Doorway test:** Mount or hold the sensor above a doorway (tape it temporarily pointing down).
   Have someone walk underneath. Observe the depth readings. Questions to answer:
   - Can you clearly distinguish a person passing through from the background floor?
   - Can you detect the direction of movement (does the person appear on one side of the grid first,
     then the other)?
   - Is 8x8 resolution sufficient, or do we need a higher-resolution sensor?

6. **Performance assessment:** Measure how fast the ESP32 can read and process frames from the
   sensor. Is it fast enough for real-time detection (~10+ fps)? How much memory does it use?

7. **Decision point:** Based on findings, decide:
   - Is the VL53L5CX sufficient or do we need a different sensor?
   - Can the ESP32 handle the processing, or do we need Pi Zeros?
   - Document the decision and reasoning.

### Success Criteria

- Sensor wired and reading depth data reliably
- Can visually confirm a person walking through a doorway shows a clear signal in the depth data
- Decision made on sensor adequacy and compute platform

---

## Phase 2: Edge Processing Pipeline (Single Node Detection Logic)

### Goal

Write the detection algorithm that runs on the edge node. It should take raw depth frames from the
sensor and output discrete events: "person entered" or "person exited" with a direction. Implement
power management for battery operation.

### Learning Objectives

- Signal processing on constrained hardware
- Algorithm design for detection and tracking
- Power management strategies (deep sleep, duty cycling)
- Battery life estimation and measurement

### Tasks

1. **Background calibration:** On startup, the sensor should capture several frames with no one in
   the doorway to establish a "baseline" depth map (the floor). Any significant deviation from this
   baseline indicates something/someone is present.

2. **Detection algorithm:** Design a simple detection pipeline:
   - Read frame → subtract background → threshold to find "occupied" zones in the 8x8 grid
   - Track the occupied zone across consecutive frames to determine direction of travel
     (left-to-right vs right-to-left across the grid = in vs out, depending on sensor orientation)
   - Debounce to avoid counting one person multiple times
   - Handle edge cases: two people passing simultaneously, someone stopping in the doorway, sensor
     noise

3. **Event generation:** When a crossing is detected, produce a structured event:

   ```json
   {
     "node_id": "doorway_01",
     "event": "crossing",
     "direction": "in",
     "timestamp": "2025-01-15T14:30:22.451Z",
     "confidence": 0.85
   }
   ```

4. **Power management:** This is critical for battery operation.
   - Research ESP32 deep sleep modes. The sensor should sleep and wake on a duty cycle.
   - Determine: can the sensor detect motion as a wake trigger, or do we need to poll periodically?
   - Calculate estimated battery life: sensor power draw + ESP32 active/sleep power draw × expected
     polling frequency → required battery capacity for 1 week runtime.
   - Select appropriate LiPo battery and charging board (TP4056 module).

5. **Local buffering:** If WiFi or MQTT is temporarily unavailable, events should be stored locally
   (in ESP32 flash/SPIFFS or SD card) and forwarded when connectivity resumes. Implement a simple
   store-and-forward buffer.

### Success Criteria

- Algorithm reliably detects a person crossing with correct direction at least 80% of the time in
  testing
- Events are generated as structured JSON
- Power budget calculated and battery hardware selected
- Store-and-forward buffering implemented and tested (simulate offline by disconnecting WiFi)

---

## Phase 3: IoT Communication Layer (MQTT & Device Management)

### Goal

Set up the MQTT infrastructure. Get the sensor node publishing events to the Pi 5 broker. Implement
basic device management: registration, heartbeats, and remote configuration.

### Learning Objectives

- MQTT protocol: topics, QoS levels, retained messages, last will and testament (LWT)
- Message broker setup and administration
- IoT device lifecycle: provisioning, health monitoring, configuration
- Network security basics (TLS for MQTT, device authentication)

### Tasks

1. **Broker setup:** Install Mosquitto MQTT broker on the Raspberry Pi 5. Configure it for local
   network access. Set up basic authentication (username/password at minimum). Optionally configure
   TLS for encrypted communication — explain why this matters even on a home network (learning
   exercise).

2. **Topic design:** Design the MQTT topic hierarchy. Suggested structure:

   ```
   home/doorways/{node_id}/events      — crossing events published by nodes
   home/doorways/{node_id}/status       — heartbeat/health from nodes
   home/doorways/{node_id}/config       — configuration pushed TO nodes (retained)
   home/system/occupancy                — aggregated occupancy (published by backend)
   ```

   Explain topic design principles: why hierarchy matters, retained messages for config, QoS level
   choices.

3. **Node MQTT client:** Implement MQTT client on the ESP32. It should:
   - Connect to broker on boot with authentication
   - Subscribe to its config topic for remote parameter updates
   - Publish crossing events to its events topic
   - Publish periodic heartbeat messages to its status topic (battery level, uptime, sensor health)
   - Use LWT (Last Will and Testament) so the broker publishes a "node offline" message if the node
     disconnects unexpectedly

4. **Remote configuration:** The node should accept configuration changes over MQTT without
   redeployment:
   - Detection sensitivity threshold
   - Polling/sampling frequency
   - Sleep duration
   - Publish these as retained messages on the config topic from the Pi 5

5. **Device registration:** When a new node comes online for the first time, it should announce
   itself. The backend should recognize new nodes and add them to its registry. Simple approach:
   node publishes a registration message with its ID, sensor type, firmware version, and location
   label.

6. **Test the full loop:** Trigger a crossing → event published → verify it arrives at the broker →
   verify heartbeats are flowing → change a config parameter remotely and confirm the node picks it
   up.

### Success Criteria

- Mosquitto running on Pi 5 with authentication
- ESP32 node publishing events and heartbeats reliably
- Remote config changes reflected on the node without physical access
- LWT working (unplug the node and verify the broker publishes the offline message)

---

## Phase 4: Second Node & Distributed Systems Challenges

### Goal

Bring the second sensor node online. Confront and solve the real distributed systems problems that
emerge: clock synchronization, event deduplication, shared-hallway double-counting, and graceful
degradation when a node fails.

### Learning Objectives

- Clock synchronization in distributed systems (NTP, logical clocks)
- Event deduplication and correlation across independent nodes
- Consistency models: what does "correct occupancy count" mean when events arrive out of order?
- Fault tolerance and graceful degradation

### Tasks

1. **Build and deploy second node:** Replicate the hardware and software from Phases 1-3 for the
   second doorway.

2. **Clock synchronization:** ESP32s have no real-time clock. They'll drift.
   - Configure NTP synchronization on both nodes
   - Discuss: why does clock drift matter? What happens if Node A's timestamp says 14:30:22 and Node
     B's says 14:30:21 but they saw the same person 3 seconds apart?
   - Implement: sync clocks via NTP on WiFi connect, include sync quality/confidence in heartbeat
     messages

3. **The hallway problem:** If two doorways are close together (e.g., hallway between kitchen and
   bedroom), the same person triggers both sensors within seconds. The backend needs to understand
   this as one person moving from room A to room B, not as two separate events.
   - Design a correlation algorithm: events from adjacent nodes within a short time window with
     opposite directions (exit from A + enter to B) likely represent the same person
   - This is a real distributed systems problem — you're doing event correlation across independent
     producers with imperfect clocks

4. **Event ordering and consistency:** Events may arrive at the broker out of order (network jitter,
   buffered events from a node that was temporarily offline).
   - Implement event ordering at the backend using timestamps
   - Handle late-arriving events: if a buffered event from 10 minutes ago arrives, how does the
     backend reconcile the occupancy count?
   - Discuss the CAP theorem at a conceptual level — this system prioritizes availability (nodes
     keep working when disconnected) and eventual consistency (counts may be temporarily wrong but
     converge)

5. **Fault tolerance:** What happens when a node goes offline?
   - The system should continue operating with reduced coverage
   - The dashboard should show which nodes are online/offline
   - When a node comes back, its buffered events should be processed without corrupting the count
   - Test this by physically unplugging a node, walking through its doorway (events are lost —
     that's okay), plugging it back in, and observing system behavior

6. **Occupancy drift correction:** Over time, counts will drift due to missed detections,
   double-counts, or unmonitored entry points. Implement a simple correction mechanism:
   - Manual reset button on the dashboard ("I know the house is empty")
   - Heuristic: if no motion detected anywhere for N hours, assume occupancy is zero

### Success Criteria

- Two nodes operating simultaneously and publishing to the same broker
- Clock sync verified (timestamps within acceptable tolerance)
- Hallway/double-counting problem addressed with correlation logic
- System continues operating when one node is unplugged
- Occupancy count is reasonably accurate over a multi-hour test

---

## Phase 5: Backend Service & Digital Twin Dashboard

### Goal

Build the backend service that consumes MQTT events and maintains the occupancy model, and the React
dashboard that visualizes it as a 2D floor plan digital twin.

### Learning Objectives

- Digital twin concept: a live virtual model of a physical space
- Backend event processing and state management
- Real-time data flow from MQTT to websocket to browser
- React application architecture for real-time data
- Representing physical space digitally (2D floor plan)

### Tasks

1. **Backend service:** Build a Python service (FastAPI or Flask) running on the Pi 5 that:
   - Subscribes to all MQTT event and status topics
   - Maintains an in-memory occupancy model (room → person count)
   - Runs the event correlation and deduplication logic from Phase 4
   - Exposes a WebSocket endpoint that pushes occupancy state changes to connected dashboards
   - Exposes a REST API for historical data, node status, manual overrides
   - Persists events to a lightweight database (SQLite is fine) for history and debugging

2. **Digital twin floor plan:** Create the 2D floor plan representation.
   - The developer will need to provide a rough layout of their two-story house (rooms, doorways,
     which nodes cover which doorways)
   - Represent this as a data structure: rooms with positions, doorways connecting rooms, sensor
     node assignments
   - The floor plan doesn't need to be architecturally precise — a schematic representation is fine

3. **React dashboard:** Build the frontend:
   - 2D floor plan view with tabs or a toggle for Floor 1 / Floor 2
   - Rooms colored or labeled by current occupancy (empty, 1 person, 2+ people)
   - Sensor node status indicators (online/offline, battery level)
   - Real-time updates via WebSocket — no polling
   - Event log showing recent crossings
   - Manual occupancy reset button
   - Node configuration panel (change sensitivity, polling rate via MQTT)
   - Responsive design (usable on phone to check from another room)

4. **Digital twin concepts to explore:**
   - The dashboard is a digital twin because it's a synchronized virtual representation of the
     physical space that updates in real time
   - Discuss: what makes this a digital twin vs just a dashboard? (The twin maintains state that
     mirrors physical reality and can be used for simulation, prediction, and control — not just
     visualization)
   - Stretch: add a simple "replay" feature that lets you scrub through historical occupancy data
     and watch the floor plan animate — this demonstrates the twin's ability to represent past
     states

### Success Criteria

- Backend consuming MQTT events and maintaining accurate occupancy state
- WebSocket pushing updates to the dashboard in real time
- Dashboard shows floor plan with live occupancy for both floors
- Node status visible on dashboard
- Event history persisted and viewable

---

## Phase 6: Polish, Battery Optimization & Production Hardening

### Goal

Take the system from "working prototype" to "runs reliably in my house for weeks." Focus on
reliability, power efficiency, enclosures, and monitoring.

### Learning Objectives

- Production vs prototype mindset
- Power optimization techniques
- System monitoring and alerting
- Physical product design considerations

### Tasks

1. **Power optimization deep dive:**
   - Profile actual power consumption of a node (this is where the multimeter is essential)
   - Optimize sleep/wake cycles based on actual usage patterns (e.g., poll more frequently during
     active hours, less at night)
   - Implement adaptive polling: if a crossing was just detected, stay awake briefly expecting more
     activity
   - Target: 1-2 weeks on a single battery charge (adjust based on battery size)

2. **Enclosure design:**
   - Design a simple enclosure for the sensor node (3D printed, or a small project box from an
     electronics store)
   - Must mount above a doorway pointing down
   - Must allow access for charging or battery swap
   - Must not block the sensor's field of view

3. **System monitoring:**
   - Dashboard alerts: node battery low, node offline for more than X minutes
   - Logging: structured logs on the Pi 5 for debugging
   - Uptime tracking: how long has each node been running continuously?

4. **Reliability improvements:**
   - Automatic reconnection on WiFi dropout
   - Watchdog timer on ESP32 (auto-reboot if firmware hangs)
   - Backend auto-restart on crash (systemd service on the Pi 5)
   - Database cleanup: don't let SQLite grow unbounded — archive or prune old events

5. **Calibration and maintenance:**
   - Recalibration routine: if the sensor is bumped or the doorway changes, re-run background
     calibration remotely via MQTT config
   - Documentation: write a short README for your own future reference — how to add a new node, how
     to recalibrate, how to check logs

### Success Criteria

- System runs unattended for at least 1 week
- Battery life meets target
- Nodes recover automatically from WiFi dropouts and crashes
- Dashboard shows accurate, real-time occupancy reliably

---

## Future Expansion Ideas (Post-MVP)

These are not part of the core project but are natural next steps once the foundation is solid:

- **More nodes:** Add sensors to remaining doorways (front door, side door, bedroom, etc.) to get
  full house coverage
- **Presence zones:** Combine doorway counting with room-level ToF sensors to detect not just
  entry/exit but where someone is within a room
- **3D digital twin:** Upgrade from 2D floor plan to a 3D model of the house (Three.js or similar)
- **Predictive patterns:** Use historical data to predict occupancy patterns ("usually someone's in
  the kitchen at 7am")
- **Integration:** Connect to Home Assistant, smart lighting, HVAC — lights turn off in unoccupied
  rooms
- **Machine learning at the edge:** Train a small model to distinguish between people, pets, and
  objects
- **OTA firmware updates:** Push new firmware to nodes over WiFi without physical access
- **Multi-sensor fusion:** Combine depth with a cheap PIR motion sensor as a low-power wake trigger
  to save battery

---

## Technical Reference

### Architecture Diagram (Text)

```
[ToF Sensor] → [ESP32 Node 1] --WiFi/MQTT-→ [Raspberry Pi 5] ←--WebSocket--→ [React Dashboard]
[ToF Sensor] → [ESP32 Node 2] --WiFi/MQTT-↗   ├── Mosquitto Broker
                                                 ├── Python Backend Service
                                                 ├── SQLite Database
                                                 └── Serves React Frontend
```

### Key Technology Choices

| Component       | Technology                   | Reason                                                       |
| --------------- | ---------------------------- | ------------------------------------------------------------ |
| Sensor          | VL53L5CX (ToF)               | Cheap, I2C, gives depth grid, works in dark                  |
| Edge compute    | ESP32 (or Pi Zero if needed) | Low power, WiFi built in, sufficient for signal processing   |
| Communication   | MQTT (Mosquitto)             | Lightweight, pub/sub, designed for IoT, supports QoS and LWT |
| Central hub     | Raspberry Pi 5               | Plenty of power for broker + backend + dashboard             |
| Backend         | Python (FastAPI)             | Developer knows Python, async support, WebSocket support     |
| Database        | SQLite                       | Zero config, sufficient for this scale                       |
| Frontend        | React                        | Developer knows React, good for real-time UI                 |
| Dashboard comms | WebSocket                    | Real-time push without polling                               |

### Useful Resources

- VL53L5CX datasheet and Arduino/ESP32 library: SparkFun and STMicroelectronics documentation
- Mosquitto MQTT broker: https://mosquitto.org/
- MQTT protocol overview: HiveMQ's MQTT Essentials blog series (excellent for learning)
- ESP32 deep sleep: Espressif documentation and Random Nerd Tutorials
- FastAPI with WebSocket: FastAPI official documentation
- Digital twin concepts: search for "digital twin IoT architecture" for theoretical grounding
