/*
 * background.h -- Per-cell background model for the VL53L5CX doorway node.
 *
 * PURE C, ZERO ESP-IDF DEPENDENCIES. The exact same source compiles for the
 * ESP32 firmware and for the host-native regression harness. No esp_*, no
 * FreeRTOS, no I2C includes here -- this module takes int dist[64] / int
 * status[64] in and reports occupancy out. All hardware coupling stays in the
 * caller (the firmware main loop or the native test driver).
 *
 * This is a faithful port of detector/background.py (the GOLDEN REFERENCE).
 * Constants and algorithm steps mirror that file 1:1. float (not double): the
 * ESP32 FPU is single-precision, so the regression test uses tolerances, not
 * bit-equality, against the Python double-precision output.
 */
#ifndef DETECTION_BACKGROUND_H
#define DETECTION_BACKGROUND_H

#include <stdbool.h>

#define BG_GRID     8
#define BG_N_CELLS  64   /* BG_GRID * BG_GRID */

/* ---- tuning knobs (mirror background.py exactly) ----------------------- */
#define BG_BOOTSTRAP_FRAMES 150    /* frames assumed empty at startup to seed bg */
#define BG_ALPHA            0.01f  /* EMA learning rate; 1/ALPHA ~= frames to adapt */
#define BG_K_OCCUPY         6.0f   /* occupied if dev > K_OCCUPY * mad   (declare) */
#define BG_K_CLEAR          4.0f   /* clears  if dev < K_CLEAR  * mad   (hysteresis) */
#define BG_MAD_FLOOR_MM     15.0f  /* min noise floor so quiet cells aren't hair-trigger */

/* Perception's own bg-update gate. Conceptually independent from the tracker's
 * TRK_MIN_BLOB_CELLS even though they currently hold the same value -- keep
 * them as two separate constants so either stage can be retuned alone. */
#define BG_MIN_BLOB_CELLS   4

/* VALID_STATUS = {5, 9, 10, 255}; HOT_PIXELS = {1, 5} -- defined as DATA in
 * background.c so they generalize to per-node auto-detection later. */

typedef struct {
    float bg[BG_N_CELLS];           /* background estimate (mm)           */
    float mad[BG_N_CELLS];          /* per-cell noise estimate (mm)       */
    bool  calibrated[BG_N_CELLS];   /* did this cell get a bootstrap seed? */
    bool  cell_hot[BG_N_CELLS];     /* per-cell hysteresis latch          */
    bool  ready;                    /* bootstrap_finalize() called?        */
    /* bootstrap sample buffers -- statically sized, no heap. At most one
     * sample per cell per bootstrap frame, so BG_BOOTSTRAP_FRAMES caps it. */
    int   boot[BG_N_CELLS][BG_BOOTSTRAP_FRAMES];
    int   boot_count[BG_N_CELLS];
} background_model_t;

void bg_init(background_model_t *m);

/* bootstrap: call for the first BG_BOOTSTRAP_FRAMES frames, then finalize. */
void bg_bootstrap_add(background_model_t *m,
                      const int dist[BG_N_CELLS],
                      const int status[BG_N_CELLS]);
void bg_bootstrap_finalize(background_model_t *m);

/* per-frame: fills occupied[] (true where cell is currently occupied) and
 * dev[] (deviation in mm, 0 where no-data). Side effect: gated EMA bg update. */
void bg_process(background_model_t *m,
                const int dist[BG_N_CELLS],
                const int status[BG_N_CELLS],
                bool occupied[BG_N_CELLS],
                float dev[BG_N_CELLS]);

/* Is cell idx usable this frame? (not hot-masked AND status trusted) */
bool bg_trusted(const int status[BG_N_CELLS], int idx);

/* 8-connected largest connected component over occ[]. Writes the component's
 * flat indices into out_cells[] and returns its size (0 if none). Shared by
 * the bg gate and the tracker (mirrors background.py label_blobs + the
 * tracker's largest_blob, which are the same routine). */
int det_largest_blob(const bool occ[BG_N_CELLS], int out_cells[BG_N_CELLS]);

#endif /* DETECTION_BACKGROUND_H */
