# Detection Logic Review — Critique & Sensor-Feature Opportunities

_Date: 2026-06-09. Scope: `doorway-node-01/src/detection/{background,tracker}.{c,h}` (and their
Python golden counterparts), plus the wake/capture state machine in `main.c`. Status: critique
only — no code changed. Each finding cites code, gives severity, and states the blast radius of
fixing it (does it invalidate the 0/8/8/4 regression?)._

Severity scale: **BUG** (wrong output possible) · **ROBUSTNESS** (degrades in conditions not yet
tested) · **DESIGN LIMIT** (known boundary of the current approach) · **NIT** (cosmetic/clarity).

---

## A. Correctness findings

### A1. `VALID_STATUS` trusts status 255 — which means "NO target detected" — **BUG (latent)**

`background.c:11` — `VALID_STATUS = {5, 9, 10, 255}`.

What the codes actually mean (UM2884 Table 4, confirmed against the ULD source in this repo):

| status | meaning |
|---|---|
| 5 | Range valid (the only 100%-confidence code) |
| 9 | Range valid with large pulse — **may be a merged target** |
| 10 | Range valid, but no target detected at previous range |
| 255 | **No target detected** |

255 is not a measurement quality — it's the driver telling you the distance field is
meaningless. `vl53l5cx_api.c:838-845` shows the mechanism: when `nb_target_detected[zone] == 0`
the driver *writes* 255 into `target_status`; the `distance_mm` slot is whatever the sensor left
there (typically 0 or stale).

Consequences in the current pipeline:

- **Bootstrap** (`background.c:135-143`): 255-cells contribute garbage distances to the median.
  A cell that frequently returns no-target gets a background built from junk samples — a
  plausible contributor to the bimodal cell (0,1) that's currently hand-masked.
- **Per-frame** (`background.c:186-204`): a 255-cell with `dist=0` yields
  `dev = bg - 0 ≈ +2000mm` → instantly latches occupied.

The subtle part — **this misfeature may be accidentally load-bearing**: IR-absorbing surfaces
(black hair, dark clothing) can return no signal at 940nm → status 255 → dist 0 → huge dev →
occupied. So 255-as-trusted acts as an unintentional "absorbing object present" detector. Naively
removing it could make the system *blind to dark-haired people*. It must be handled
**deliberately**, not removed blindly:

- Recommended shape: treat 255 as its own tri-state — *excluded from bootstrap and EMA learning*
  (it's not a distance), but *counted as occupied-evidence* in a cell whose background says
  "floor normally returns here" (a normally-returning cell suddenly returning nothing = something
  absorbing is in front of it).
- **First step is data, not code**: log a per-cell status histogram on hardware (empty doorway,
  then crossings with dark and light clothing). The fixtures also contain the raw status streams —
  replay them with 255 removed from `VALID_STATUS` in `background.py` and diff events to measure
  how much of current behavior depends on it.

Blast radius: bootstrap + occupancy semantics → full re-run of Python replay + native regression;
fixtures may legitimately change goldens. Do not bundle with other changes.

### A2. Hysteresis latch destroyed by one untrusted frame — **ROBUSTNESS**

`background.c:187-190`: if a cell's status goes untrusted for a single frame,
`cell_hot[i] = false` — the occupied latch is wiped, `dev=0`, the cell drops out of the blob.

A person directly under the sensor is exactly the situation that produces marginal statuses
(strong reflection → wrap/consistency errors; dark surface → low signal). So mid-crossing, cells
on the person can blink out, fragmenting the blob. Today this is absorbed by 8-connectivity +
`TRK_GRACE_FRAMES`, and partially hidden by A1 (255 being "trusted" keeps absorbing cells in).
Fixing A1 makes A2 *more* exposed — they're coupled.

Fix shape: on untrusted status, **hold** the latch (skip the update entirely) instead of clearing
it; let `K_CLEAR` hysteresis do its job on the next trusted frame. One-line change, conceptually.
Blast radius: same as A1 (re-run replay + regression); should be tuned/validated together.

### A3. `TRK_MAX_TRACK_FRAMES` force-close can double-count a slow crossing — **BUG (edge case)**

`tracker.c:181-183`: at 100 occupied frames (~10s) the track is force-closed *and classified* —
then the very next occupied frame opens a fresh track from mid-doorway.

Walk-through with a >10s pause: track 1 = entry→center, net ≈ 2.5 cells ≥ `MIN_NET_DISPLACEMENT`
→ **emits** (direction of the half-crossing); track 2 = center→exit → **emits again, same
direction** → occupancy off by one, permanently (this is exactly the drift class the project
fights everywhere else). The stop-in-doorway fixture (4–6s pauses) passes only because its pauses
sit under the 10s cap.

Root cause is architectural, and the fix is nice: the classifier only ever uses **first-k/last-k
centroids + path-length accumulator + peak size** (`tracker_close`, `tracker.c:56-113`). None of
that needs the full centroid history. Keep a k-deep head buffer, a k-deep ring for the tail, and
running accumulators → **O(1) memory, no cap, no force-close, no buffer at all**. The 100-frame
cap currently exists *to size the buffer*; remove the buffer and the bug's reason to exist goes
with it. (Keep a sanity timeout that *discards* — never classifies — a pathological never-ending
track, e.g. furniture; see A4.)

Blast radius: tracker internals rewrite, but identical outputs for any track ≤100 frames →
existing goldens must pass unchanged. Add a new synthetic long-pause fixture to prove the fix.

### A4. Persistent foreground = permanent detector corruption, no recovery — **ROBUSTNESS**

`background.c:219-225`: bg learns only from frames whose largest blob < `BG_MIN_BLOB_CELLS`.
Correct for people — but if the scene *actually changes* (package left in the doorway, door
half-closed, cat bed moved), that object's cells are occupied forever:

- bg never absorbs it (frame gate blocks learning),
- every capture burst tracks *the largest blob* — which may now be the object, not the person
  (`main.c` capture loop → `det_largest_blob`), so real crossings get missed or misread,
- bursts run to `CAPTURE_MAX_FRAMES` every wake.

No recovery short of a reboot with a clear doorway. Classic background-subtraction problem with a
classic answer: **absorption timeout** — a cell continuously hot for N minutes (people cross in
seconds) gets folded into bg (re-seed that cell from current readings). Plus the already-planned
Phase 6 remote-recalibrate MQTT command as the manual override. Cheap insurance:
ESP_LOGW when a burst ends by MAX_FRAMES with a persistent blob — that's the "scene changed"
signature, visible in the heartbeat if you add a flag.

Blast radius: additive (new timeout path); fixtures unaffected (no fixture runs minutes-long
occupancy). Device-test only.

### A5. Confidence ignores how much of the crossing was actually seen — **NIT→ROBUSTNESS**

`tracker.c:99-106`: confidence = straightness (net/path) only. A clipped 4-frame track that
happens to be straight reports confidence ~0.9; a fully-observed 20-frame crossing with natural
sway reports ~0.7. Downstream (Phase 5 backend) will want to weight events by quality; today's
confidence inverts it. Fold in track length and peak blob size (e.g. multiply by
`min(1, n_centroids/8)`). Blast radius: confidence values change → golden tolerance fields;
direction/event counts untouched.

### A6. Minor consistencies — **NIT**

- `tracker.c:51`: `TRK_MIN_TRACK_FRAMES` compares `n_centroids`, while `ev.frames` counts
  lifecycle frames incl. grace (`tracker.c:178`,`191`) — two subtly different "frame counts"
  travel under one name. Rename fields, or document.
- `blob_centroid` skips `w <= 0` cells (`tracker.c:19`) — currently unreachable (a hot cell this
  frame implies `dev ≥ K_CLEAR·mad > 0`). Fine as defense; worth a comment so nobody "fixes" it.
- `BG_MAD_FLOOR_MM=15` with `K_OCCUPY=6` → minimum trip threshold 90mm. Sensible vs the ~900mm
  person signal; just record that pets/objects below ~90mm deviation are invisible *by accident*
  of these two constants, not by a deliberate height gate (see B3).

---

## B. Design limits

### B1. Largest-blob-only + single track = two people are invisible as two — **DESIGN LIMIT**

Known/deferred since Phase 2. Three layers all assume one person: blob selection
(`det_largest_blob` keeps exactly one component), track lifecycle (one `track_t`), direction
(one net vector). Two people abreast = one merged blob = one event. Two people opposite
directions = net ≈ 0 = zero events (worse than double-count: silent). When this becomes real
(Phase 4+), the cheap ladder is: (1) track the two largest blobs; (2) sensor-side multi-target
(D3) to split overhead-merged returns; (3) only then real multi-object tracking.

### B2. `in_axis` is a hand-derived constant — **DESIGN LIMIT**

`tracker.h:35-36`, re-derived offline from fixture walks every time gate constants change, must
be redone per node/remount. Two practical upgrades: (a) it's now MQTT-config-ready — make it a
config field like `motion_threshold` (was the Phase 3 promise; not yet wired); (b) self-calibration
mode: node collects N crossings labeled by the user via dashboard ("that was an exit"), derives
in_axis on the Pi, pushes it back. Removes the laptop from the loop before node 2 arrives.

### B3. No magnitude gate — a cat is a person if it's blobby enough — **DESIGN LIMIT**

Occupancy is purely geometric (≥4 connected cells over per-cell noise). A dog at 400mm deviation
sweeps the same blob pattern as a person at 900mm. Phase 1 data shows people at 900+mm; a
`MIN_PEAK_DEV` gate (e.g. requires *some* cell in the blob > ~500mm) would filter floor-level
animals while keeping crawling toddlers detectable (judgment call to record either way). Needs
real pet captures before tuning — collect fixtures first.

### B4. Bootstrap trusts the doorway to be clear, and never checks — **ROBUSTNESS**

`bg_bootstrap_add` accepts all 150 frames unconditionally. Median survives a *walk-through*, but
someone standing in the doorway for >75 frames poisons that cell's background (and its MAD
explodes or collapses). Cheap check at finalize: a cell whose bootstrap MAD is wildly above its
neighbors, or whose median deviates grossly from a plane-ish floor prior, gets flagged
uncalibrated → auto-extend bootstrap. Also the natural home for **auto hot-pixel detection**
(deferred Phase 2 debt): the hand-coded `HOT_PIXELS = {1, 5}` mask is exactly "cells whose
bootstrap statistics look broken" — node 2 will need this anyway; its defect map will differ.

---

## C. Wake tier / state machine (`main.c`)

### C1. The directional clipping bug — root-cause analysis — **BUG (open since Phase 2)**

Observed: "in" crossings emit; "out" crossings produce a solid blob whose track dies on
`net_too_small` / `too_few_frames`. Latency chain on wake: motion indicator must accumulate
frame-to-frame variation (≥1-2 autonomous frames @10Hz) → INT → ESP32 ACK + validate (poll up to
25ms + I2C read) → `sensor_start_continuous()` (stop → reconfigure → start: several frames lost)
→ first detector frame. Total ≈ 300-600ms — at normal walking pace that's 1-2 grid cells of
travel already gone. The asymmetry is geometric: one walking direction enters the FOV corner that
the (rotated, diagonal-travel) grid sees *last*, so the surviving observable arc is shorter for
"out" — under the latency, only that direction's track falls below the gates.

The three knobs already identified in the Decisions Log (raise `ARMED_FREQ_HZ`, trim mode-switch,
live with it) are all latency *reductions*. Two better options eliminate latency classes
entirely:

### C2. Feed the wake frame into the detector — **free, currently thrown away**

The ARMED ACK path (`main.c` wake handling) *already reads a full ranging frame*
(`vl53l5cx_get_ranging_data`) to clear INT and validate motion — then discards it. That frame
contains the person's position at wake. Run `bg_process` + `tracker_update` on it before the mode
switch and the track starts 1 frame earlier *plus* survives the mode-switch gap via grace frames.
Zero extra sensor traffic, zero config change, pure firmware. Caveat to verify: autonomous-mode
frames at `ARMED_INTEGRATION_MS=10` may be noisier than continuous frames — check status quality
of wake frames on hardware first.

### C3. Wake on DISTANCE, not motion — **removes the motion-estimation latency entirely**

The detection-threshold plugin supports more than the motion indicator
(`vl53l5cx_plugin_detection_thresholds.h:50-55`): checkers can fire on **`DISTANCE_MM`** with
`IN_WINDOW` type (`.h:65`), and checkers **combine with OR/AND** (`.h:78-80`).

A distance-window checker (`IN_WINDOW`, 500–1900mm — same window the motion config uses) fires
the instant *any zone sees a return in person-height range* — first frame the person breaks the
plane, no frame-to-frame motion accumulation needed. Static-scene false positives (furniture) are
handled by the absorption fix (A4) and the existing wake-validation gate. Can be OR'd with the
motion checker during a trial period to compare wake latencies empirically. This is the highest-
leverage clipping fix and it's pure configuration.

### C4. Re-test whether INT really is autonomous-only — **cheap experiment, big prize**

The Phase 2 empirical finding "INT only fires in autonomous mode" contradicts UM2884/ULD docs,
which describe INT pulsing on every data-ready in continuous mode too. If INT *does* pulse
per-frame in continuous (possibly the earlier test mis-wired thresholds-enable), the entire
two-tier architecture has an alternative: **sensor stays in continuous 10Hz forever; ESP32
light-sleeps between frames and wakes on INT; detector sees every frame** → clipping is
impossible by construction, mode-switch code deleted. Cost: sensor continuous draw
(~mid-tens of mW) vs autonomous — measure against the multimeter when it arrives (Phase 6).
Worth one evening: enable INT in continuous, scope/log whether it pulses.

### C5. 4×4 @ 60Hz as a capture mode — **situational, probably not needed**

6× temporal resolution at ¼ spatial. Phase 1 showed 8×8@10Hz gives 8-20 frames/crossing —
adequate once clipping is fixed at the source (C2/C3). Keep in the back pocket for genuinely
fast events (running children); it would require full re-tune (blob gate, MAD, in_axis,
fixtures) — expensive, low expected return today.

---

## D. Unused sensor features — ranked

All result fields below are **already enabled and arriving in every frame** (default ULD build,
`platform.h:52` sets `VL53L5CX_NB_TARGET_PER_ZONE=1`; none of the `VL53L5CX_DISABLE_*` macros are
set) — the firmware just ignores them.

| # | Feature | What it buys | Cost | Addresses |
|---|---------|--------------|------|-----------|
| D1 | `DISTANCE_MM` + `IN_WINDOW` wake checker, OR-able with motion | Eliminates motion-accumulation wake latency | Config only | C1/C3 |
| D2 | Wake-frame backfill (not a sensor feature — an already-read frame) | 1 extra track frame + grace bridge | ~10 lines | C1/C2 |
| D3 | `range_sigma_mm` (per-zone noise estimate, mm) | Sensor's own per-frame noise → could replace/augment bootstrap MAD; per-frame trust weighting (centroid weight ∝ dev/sigma) | Medium: plumb through `bg_process` | A1/A2, better centroids |
| D4 | `signal_per_spad` (return strength) | Direct "absorbing target" detection (low signal + valid-ish status); cleaner solution to the 255-of-dark-hair problem than trusting 255 | Medium | A1 |
| D5 | Multi-target (`CONFIG_VL53L5CX_NB_TARGET_PER_ZONE=2`, Kconfig) | Splits merged overhead returns (head vs floor in one zone at blob edges; two-people) | High: doubles results payload, per-target indexing everywhere, re-tune | B1 |
| D6 | Sharpener (default 5%, 0-99%) | Higher % = less zone bleed = crisper blob edges; could tame edge-cell flicker (bimodal (0,1)) | Config + re-validate fixtures | B4, edge cells |
| D7 | `reflectance` (% of emitted light returned) | Possible person/pet/material discrimination signal | Low to log, unknown value | B3 (speculative) |
| D8 | Crosstalk calibration (`vl53l5cx_calibrate_xtalk`) | Required when Phase 6 enclosure puts glass over the sensor | Phase 6 | — |

---

## E. Prioritized recommendations

Ordered by leverage ÷ risk. Items 1-3 are independent of each other; 4-5 are coupled.

1. **Instrument before touching anything** (zero risk): add a per-cell status histogram +
   per-frame `min(signal_per_spad)` log mode (or extend the FRAME line). One evening of captures:
   empty, light clothing, dark clothing, (a pet if available). This data decides A1, A2, B3, D4
   factually instead of by argument. _No regression impact._
2. **Wake-frame backfill (C2/D2)**: feed the already-read wake frame to the detector. ~10 lines in
   `main.c`, device-only, directly attacks the open clipping bug. _No fixture/golden impact._
3. **Distance-window wake checker OR'd with motion (C3/D1)**: config-level change in
   `sensor_arm_autonomous`. Trial both wake paths, log which fires first. _No fixture impact._
   Expected outcome: C1 clipping closed; if not, run the C4 experiment before any architecture
   rework.
4. **Status-semantics overhaul (A1+A2 together)**: tri-state 255 handling + latch-hold on
   untrusted. Gate on the data from item 1. _Re-runs Python replay + native regression; goldens
   may change → this is its own validated change-set, like the gate=5→4 episode._
5. **O(1) tracker endpoints, delete force-close (A3)**: removes the double-count edge and the
   100-frame cap. Goldens must pass byte-identical for existing fixtures; add one synthetic
   long-pause fixture. _Mechanical but touches the validated tracker — do alone._
6. **Absorption timeout + scene-change flag (A4)** before the nodes run unattended for days
   (pre-Phase 6). Device-tested.
7. **in_axis → MQTT config (B2)**: finish the Phase 3 promise; prerequisite for node 2 install.
8. **Auto bad-cell detection at bootstrap (B4)**: build when node 2 arrives and its defect map
   proves the point.
9. **Two-people (B1/D5)**: stays deferred until real-world observation says otherwise — correct
   call.

### Offline proof commands (for items 4-5)

The fixtures carry raw per-frame status, so A1/A2 are testable without hardware: edit
`VALID_STATUS` / latch behavior in `detector/background.py`, re-run the replay harness over
`fixtures/` (same procedure as the Phase 2 validation), diff event lists; then port and run
`detector/native/run_regression.py` against regenerated goldens.

---

## Bottom line

The Phase 2 core is genuinely sound: the median+MAD background, gated EMA, hysteresis,
connectivity-based detection, and net-displacement direction are all defensible, validated
choices. The real exposure is at the **edges of the data contract** — what status 255 means, what
happens when status flickers, what happens after frame 100, what happens when the scene changes —
and in the **wake tier**, where the open clipping bug has two cheap, untried fixes (C2, C3)
before any architectural rethink is justified. Nothing here demands urgency; items 1-3 are the
high-leverage, low-risk start.
