/*
 * background.c -- faithful C port of detector/background.py.
 * See background.h for the contract. Pure C, no ESP-IDF / FreeRTOS / I2C.
 */
#include "background.h"

#include <stdlib.h>   /* qsort */
#include <math.h>     /* fabsf */

/* ---- masks / trusted status, kept as DATA (Phase 1 findings #1, #4) ----- */
static const int VALID_STATUS[] = {5, 9, 10, 255};
static const int N_VALID_STATUS = (int)(sizeof(VALID_STATUS) / sizeof(VALID_STATUS[0]));

static const int HOT_PIXELS[] = {1, 5};  /* (0,1) bimodal flicker, (0,5) stuck hot */
static const int N_HOT_PIXELS = (int)(sizeof(HOT_PIXELS) / sizeof(HOT_PIXELS[0]));

bool bg_trusted(const int status[BG_N_CELLS], int idx)
{
    for (int h = 0; h < N_HOT_PIXELS; h++) {
        if (idx == HOT_PIXELS[h]) {
            return false;
        }
    }
    int s = status[idx];
    for (int v = 0; v < N_VALID_STATUS; v++) {
        if (s == VALID_STATUS[v]) {
            return true;
        }
    }
    return false;
}

/* ---- median helpers (plain, no numpy) ---------------------------------- */
static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* median of an int array (sorts it in place); n must be >= 1 */
static float median_int(int *vals, int n)
{
    qsort(vals, (size_t)n, sizeof(int), cmp_int);
    int mid = n / 2;
    if (n % 2 == 1) {
        return (float)vals[mid];
    }
    return 0.5f * ((float)vals[mid - 1] + (float)vals[mid]);
}

/* median of a float array (sorts it in place); n must be >= 1 */
static float median_float(float *vals, int n)
{
    qsort(vals, (size_t)n, sizeof(float), cmp_float);
    int mid = n / 2;
    if (n % 2 == 1) {
        return vals[mid];
    }
    return 0.5f * (vals[mid - 1] + vals[mid]);
}

/* ---- connectivity ------------------------------------------------------ */
int det_largest_blob(const bool occ[BG_N_CELLS], int out_cells[BG_N_CELLS])
{
    bool seen[BG_N_CELLS] = {false};
    int  stack[BG_N_CELLS];
    int  comp[BG_N_CELLS];
    int  best_n = 0;

    for (int start = 0; start < BG_N_CELLS; start++) {
        if (!occ[start] || seen[start]) {
            continue;
        }
        int comp_n = 0;
        int top = 0;
        stack[top++] = start;
        seen[start] = true;
        while (top > 0) {
            int cell = stack[--top];
            comp[comp_n++] = cell;
            int row = cell / BG_GRID;
            int col = cell % BG_GRID;
            /* 8-connected neighborhood (incl. diagonals) -- finding #5 */
            for (int dr = -1; dr <= 1; dr++) {
                for (int dc = -1; dc <= 1; dc++) {
                    if (dr == 0 && dc == 0) {
                        continue;
                    }
                    int nr = row + dr;
                    int nc = col + dc;
                    if (nr < 0 || nr >= BG_GRID || nc < 0 || nc >= BG_GRID) {
                        continue;
                    }
                    int nb = nr * BG_GRID + nc;
                    if (occ[nb] && !seen[nb]) {
                        seen[nb] = true;
                        stack[top++] = nb;
                    }
                }
            }
        }
        if (comp_n > best_n) {
            best_n = comp_n;
            for (int i = 0; i < comp_n; i++) {
                out_cells[i] = comp[i];
            }
        }
    }
    return best_n;
}

/* ---- lifecycle --------------------------------------------------------- */
void bg_init(background_model_t *m)
{
    for (int i = 0; i < BG_N_CELLS; i++) {
        m->bg[i] = 0.0f;
        m->mad[i] = 0.0f;
        m->calibrated[i] = false;
        m->cell_hot[i] = false;
        m->boot_count[i] = 0;
    }
    m->ready = false;
}

void bg_bootstrap_add(background_model_t *m,
                      const int dist[BG_N_CELLS],
                      const int status[BG_N_CELLS])
{
    for (int i = 0; i < BG_N_CELLS; i++) {
        if (bg_trusted(status, i)) {
            int c = m->boot_count[i];
            if (c < BG_BOOTSTRAP_FRAMES) {
                m->boot[i][c] = dist[i];
                m->boot_count[i] = c + 1;
            }
        }
    }
}

void bg_bootstrap_finalize(background_model_t *m)
{
    for (int i = 0; i < BG_N_CELLS; i++) {
        int n = m->boot_count[i];
        if (n == 0) {
            m->calibrated[i] = false;
            continue;
        }
        float med = median_int(m->boot[i], n);  /* sorts boot[i] in place */
        float devs[BG_BOOTSTRAP_FRAMES];
        for (int j = 0; j < n; j++) {
            devs[j] = fabsf((float)m->boot[i][j] - med);
        }
        float mad = median_float(devs, n);
        m->bg[i] = med;
        m->mad[i] = (mad > BG_MAD_FLOOR_MM) ? mad : BG_MAD_FLOOR_MM;
        m->calibrated[i] = true;
    }
    /* free the bootstrap buffers (reset counts; memory is static here) */
    for (int i = 0; i < BG_N_CELLS; i++) {
        m->boot_count[i] = 0;
    }
    m->ready = true;
}

void bg_process(background_model_t *m,
                const int dist[BG_N_CELLS],
                const int status[BG_N_CELLS],
                bool occupied[BG_N_CELLS],
                float dev[BG_N_CELLS])
{
    /* Caller must have finalized. (No exceptions in C: just no-op-safe guard.) */
    for (int i = 0; i < BG_N_CELLS; i++) {
        dev[i] = 0.0f;
        occupied[i] = false;
    }
    if (!m->ready) {
        return;
    }

    for (int i = 0; i < BG_N_CELLS; i++) {
        if (!m->calibrated[i] || !bg_trusted(status, i)) {
            m->cell_hot[i] = false;
            continue;
        }
        float d = m->bg[i] - (float)dist[i];  /* positive = closer than bg = object */
        dev[i] = d;
        float hi = BG_K_OCCUPY * m->mad[i];
        float lo = BG_K_CLEAR * m->mad[i];
        /* hysteresis: high bar to turn on, low bar to turn off */
        if (m->cell_hot[i]) {
            if (d < lo) {
                m->cell_hot[i] = false;
            }
        } else {
            if (d > hi) {
                m->cell_hot[i] = true;
            }
        }
        if (m->cell_hot[i]) {
            occupied[i] = true;
        }
    }

    /* BLOB GATE: a frame is occupied only if its largest connected blob is big
     * enough. A lone size-1 cell (e.g. the bimodal (0,1) cell) never trips it,
     * so bg keeps learning everywhere else. */
    int scratch[BG_N_CELLS];
    int largest = det_largest_blob(occupied, scratch);
    bool frame_occupied = largest >= BG_MIN_BLOB_CELLS;

    /* GATE: learn bg only from frames that look empty, AND never fold a cell
     * into bg while that cell is currently flagged hot. */
    if (!frame_occupied) {
        for (int i = 0; i < BG_N_CELLS; i++) {
            if (m->calibrated[i] && bg_trusted(status, i) && !m->cell_hot[i]) {
                m->bg[i] = (1.0f - BG_ALPHA) * m->bg[i] + BG_ALPHA * (float)dist[i];
            }
        }
    }
}
