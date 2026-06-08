/*
 * tracker.h -- Temporal layer: per-frame largest-blob centroids -> crossing
 * events. Faithful C port of detector/tracker.py.
 *
 * PURE C, ZERO ESP-IDF DEPENDENCIES (same contract as background.h). Fixed-size
 * track buffer (TRK_MAX_TRACK_FRAMES), no heap growth in the hot path.
 *
 * PIPELINE: bg_process() -> det_largest_blob() -> tracker_update().
 *
 * DIRECTION MODEL: travel is diagonal across the grid (Phase 1 finding #5), so
 * in/out is the projection of the track's net displacement onto a per-node
 * in_axis vector calibrated once at install -- NOT a grid axis, NOT PCA.
 */
#ifndef DETECTION_TRACKER_H
#define DETECTION_TRACKER_H

#include <stdbool.h>

#include "background.h"   /* det_largest_blob, BG_N_CELLS */

#define TRK_GRID     8
#define TRK_N_CELLS  64

/* ---- tuning knobs (mirror tracker.py exactly) ------------------------- */
#define TRK_MIN_BLOB_CELLS       4     /* gate: track is occupied if largest blob >= this */
#define TRK_GRACE_FRAMES         3     /* bridge brief gate dropouts without splitting a track */
#define TRK_MAX_TRACK_FRAMES     100   /* force-close runaway tracks; also sizes the buffer */
#define TRK_MIN_NET_DISPLACEMENT 2.0f  /* cells; below this, loiter/jitter -> no event */
#define TRK_MIN_TRACK_FRAMES     3     /* reject ultra-short tracks */
#define TRK_ENDPOINT_AVG         3     /* average first/last N centroids for robust endpoints */

/* Per-node / per-mount install constant. Re-derived from the gate=4 source on
 * the walks fixture. MUST be recalibrated if the sensor is remounted; will
 * become MQTT-configurable in Phase 3. Keep in sync with gen_golden.py IN_AXIS. */
#define IN_AXIS_X (-0.456f)
#define IN_AXIS_Y (-0.890f)

typedef enum {
    DIR_NONE = -1,
    DIR_OUT  = 0,
    DIR_IN   = 1
} direction_t;

typedef struct {
    bool        valid;       /* true only for a real emitted crossing */
    direction_t direction;
    long long   t_start;
    long long   t_end;
    int         frames;      /* lifecycle frame count (incl. grace frames) */
    float       dx, dy;      /* net displacement components (cells) */
    float       net;         /* |displacement| (cells) */
    float       path;        /* summed path length (cells) */
    float       confidence;  /* 0..1 (straightness) */
    int         peak_blob;   /* peak largest-blob size over the track */
} crossing_event_t;

typedef struct {
    float     cx[TRK_MAX_TRACK_FRAMES];
    float     cy[TRK_MAX_TRACK_FRAMES];
    int       sizes[TRK_MAX_TRACK_FRAMES];
    int       n_centroids;   /* stored centroids (<= TRK_MAX_TRACK_FRAMES) */
    long long t_start;
    long long t_end;
    int       frames;        /* occupied-frame count for this track */
    bool      active;
} track_t;

typedef struct {
    track_t track;
    int     grace;
} tracker_t;

void tracker_init(tracker_t *tk);

/* Feed one frame. blob_cells/blob_n come from det_largest_blob(); dev[] from
 * bg_process(). t is an opaque timestamp (fixture 'seq' in replay; esp_timer
 * microseconds on device) -- it only flows into t_start/t_end, never into the
 * detection logic. Returns an event with .valid=true if a crossing closed. */
crossing_event_t tracker_update(tracker_t *tk,
                                const int *blob_cells, int blob_n,
                                const float dev[TRK_N_CELLS],
                                long long t);

/* Close any open track at end of stream. */
crossing_event_t tracker_flush(tracker_t *tk);

#endif /* DETECTION_TRACKER_H */
