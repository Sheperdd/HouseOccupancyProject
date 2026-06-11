/*
 * tracker.c -- faithful C port of detector/tracker.py.
 * See tracker.h for the contract. Pure C, no ESP-IDF / FreeRTOS / I2C.
 */
#include "tracker.h"

#include <math.h>   /* sqrtf */

/* Deviation-weighted centroid of a blob -> (cx, cy) fractional grid coords.
 * Weighting by dev tracks the closest point (head/shoulders, highest SNR) and
 * damps fringe-cell flicker. Returns false (no centroid) if total weight <= 0. */
static bool blob_centroid(const int *cells, int n, const float dev[TRK_N_CELLS],
                          float *cx, float *cy)
{
    float wsum = 0.0f, cxsum = 0.0f, cysum = 0.0f;
    for (int k = 0; k < n; k++) {
        int i = cells[k];
        float w = dev[i];
        if (w <= 0.0f) {
            continue;
        }
        int r = i / TRK_GRID;
        int c = i % TRK_GRID;
        wsum += w;
        cxsum += w * (float)c;
        cysum += w * (float)r;
    }
    if (wsum <= 0.0f) {
        return false;
    }
    *cx = cxsum / wsum;
    *cy = cysum / wsum;
    return true;
}

/* Classify a finished track. Returns event (.valid set accordingly). */
static crossing_event_t tracker_close(tracker_t *tk)
{
    crossing_event_t ev;
    ev.valid = false;
    ev.direction = DIR_NONE;

    track_t *tr = &tk->track;
    bool was_active = tr->active;
    tr->active = false;
    tk->grace = 0;

    int n = tr->n_centroids;
    tk->last_n_centroids = n;
    tk->last_net = 0.0f;
    if (!was_active || n < TRK_MIN_TRACK_FRAMES) {
        tk->last_reason = was_active ? TRK_CLOSE_TOO_FEW_FRAMES : TRK_CLOSE_NOT_ACTIVE;
        return ev;
    }

    /* robust endpoints: mean of first/last k centroids, k capped at n/2 so the
     * windows never overlap (overlap understates net on short tracks). */
    int k = (TRK_ENDPOINT_AVG < n / 2) ? TRK_ENDPOINT_AVG : n / 2;
    if (k < 1) {
        k = 1;
    }
    float sx = 0.0f, sy = 0.0f, ex = 0.0f, ey = 0.0f;
    for (int i = 0; i < k; i++) {
        sx += tr->cx[i];
        sy += tr->cy[i];
    }
    sx /= (float)k;
    sy /= (float)k;
    for (int i = n - k; i < n; i++) {
        ex += tr->cx[i];
        ey += tr->cy[i];
    }
    ex /= (float)k;
    ey /= (float)k;

    float dx = ex - sx;
    float dy = ey - sy;
    float net = sqrtf(dx * dx + dy * dy);
    tk->last_net = net;

    /* path length (for straightness / confidence) */
    float path = 0.0f;
    for (int i = 1; i < n; i++) {
        float ax = tr->cx[i] - tr->cx[i - 1];
        float ay = tr->cy[i] - tr->cy[i - 1];
        path += sqrtf(ax * ax + ay * ay);
    }

    /* debounce: loiter-and-return or jitter -> no crossing */
    if (net < TRK_MIN_NET_DISPLACEMENT) {
        tk->last_reason = TRK_CLOSE_NET_TOO_SMALL;
        return ev;
    }

    /* direction via projection onto in_axis */
    float s = dx * IN_AXIS_X + dy * IN_AXIS_Y;
    direction_t direction = (s > 0.0f) ? DIR_IN : DIR_OUT;

    float straightness = (path > 0.0f) ? (net / path) : 0.0f;
    float confidence = straightness;
    if (confidence < 0.0f) {
        confidence = 0.0f;
    }
    if (confidence > 1.0f) {
        confidence = 1.0f;
    }

    int peak = 0;
    for (int i = 0; i < n; i++) {
        if (tr->sizes[i] > peak) {
            peak = tr->sizes[i];
        }
    }

    tk->last_reason = TRK_CLOSE_EMITTED;
    ev.valid = true;
    ev.direction = direction;
    ev.t_start = tr->t_start;
    ev.t_end = tr->t_end;
    ev.frames = tr->frames;
    ev.dx = dx;
    ev.dy = dy;
    ev.net = net;
    ev.path = path;
    ev.confidence = confidence;
    ev.peak_blob = peak;
    return ev;
}

void tracker_init(tracker_t *tk)
{
    tk->track.active = false;
    tk->track.n_centroids = 0;
    tk->track.frames = 0;
    tk->track.t_start = 0;
    tk->track.t_end = 0;
    tk->grace = 0;
    tk->last_reason = TRK_CLOSE_NONE;
    tk->last_net = 0.0f;
    tk->last_n_centroids = 0;
}

crossing_event_t tracker_update(tracker_t *tk,
                                const int *blob_cells, int blob_n,
                                const float dev[TRK_N_CELLS],
                                long long t)
{
    crossing_event_t none;
    none.valid = false;
    none.direction = DIR_NONE;

    int size = blob_n;
    bool occupied = size >= TRK_MIN_BLOB_CELLS;

    if (occupied) {
        float cx = 0.0f, cy = 0.0f;
        bool has = blob_centroid(blob_cells, blob_n, dev, &cx, &cy);

        if (!tk->track.active) {
            tk->track.active = true;
            tk->track.t_start = t;
            tk->track.t_end = t;
            tk->track.frames = 0;
            tk->track.n_centroids = 0;
        }
        /* _Track.add: append centroid+size only if centroid valid; always
         * advance t_end and frames. */
        if (has) {
            int idx = tk->track.n_centroids;
            if (idx < TRK_MAX_TRACK_FRAMES) {
                tk->track.cx[idx] = cx;
                tk->track.cy[idx] = cy;
                tk->track.sizes[idx] = size;
                tk->track.n_centroids = idx + 1;
            }
        }
        tk->track.t_end = t;
        tk->track.frames++;
        tk->grace = 0;

        if (tk->track.frames >= TRK_MAX_TRACK_FRAMES) {
            return tracker_close(tk);
        }
        return none;
    }

    /* not occupied this frame */
    if (tk->track.active) {
        tk->grace++;
        tk->track.t_end = t;
        if (tk->grace > TRK_GRACE_FRAMES) {
            return tracker_close(tk);
        }
    }
    return none;
}

crossing_event_t tracker_flush(tracker_t *tk)
{
    if (tk->track.active) {
        return tracker_close(tk);
    }
    crossing_event_t none;
    none.valid = false;
    none.direction = DIR_NONE;
    return none;
}
