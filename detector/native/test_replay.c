/*
 * test_replay.c -- host-native regression driver for the C detector port.
 *
 * Compiles the SAME background.c + tracker.c the ESP32 firmware uses, feeds a
 * fixture CSV through the pipeline exactly as the device will, and prints one
 * normalized line per emitted crossing event:
 *
 *     <dir>,<frames>,<dx>,<dy>,<net>,<conf>,<peak>,<t_start>
 *
 * This format matches detector/native/gen_golden.py so the two can be diffed.
 * This file is NATIVE-ONLY (never flashed): stdio/file I/O is fine here.
 *
 * CSV layout (positional): host_ts, seq, esp_t_us, d00..d63, s00..s63.
 * The tracker timestamp is the 'seq' column (col 1) -- same as the Python
 * harness, so t_start values line up.
 *
 * Usage: replay <fixture>.frames.csv
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "background.h"
#include "tracker.h"

#define LINE_MAX_LEN 8192
#define N_COLS_EXPECTED (3 + 2 * BG_N_CELLS)  /* 131 */

/* Tokenize one CSV line into seq + 64 dist + 64 status. Returns column count. */
static int parse_row(char *line, long long *seq, int dist[BG_N_CELLS], int status[BG_N_CELLS])
{
    int col = 0;
    for (char *tok = strtok(line, ",\r\n"); tok != NULL; tok = strtok(NULL, ",\r\n")) {
        if (col == 1) {
            *seq = atoll(tok);
        } else if (col >= 3 && col < 3 + BG_N_CELLS) {
            dist[col - 3] = atoi(tok);
        } else if (col >= 3 + BG_N_CELLS && col < 3 + 2 * BG_N_CELLS) {
            status[col - 3 - BG_N_CELLS] = atoi(tok);
        }
        col++;
    }
    return col;
}

static void print_event(const crossing_event_t *e)
{
    const char *d = (e->direction == DIR_IN) ? "in"
                  : (e->direction == DIR_OUT) ? "out" : "none";
    printf("%s,%d,%.3f,%.3f,%.3f,%.3f,%d,%lld\n",
           d, e->frames, e->dx, e->dy, e->net, e->confidence,
           e->peak_blob, e->t_start);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <fixture>.frames.csv\n", argv[0]);
        return 2;
    }

    FILE *fp = fopen(argv[1], "r");
    if (!fp) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    /* static: the bg model holds a large bootstrap buffer; keep it off the stack */
    static background_model_t bg;
    static tracker_t tk;
    bg_init(&bg);
    tracker_init(&tk);

    char line[LINE_MAX_LEN];

    /* skip header */
    if (!fgets(line, sizeof line, fp)) {
        fprintf(stderr, "empty file %s\n", argv[1]);
        fclose(fp);
        return 2;
    }
    long pos_after_header = ftell(fp);

    /* pass 1: bootstrap on the first BG_BOOTSTRAP_FRAMES data rows */
    int added = 0;
    while (added < BG_BOOTSTRAP_FRAMES && fgets(line, sizeof line, fp)) {
        long long seq = 0;
        int dist[BG_N_CELLS], status[BG_N_CELLS];
        if (parse_row(line, &seq, dist, status) < N_COLS_EXPECTED) {
            continue;
        }
        bg_bootstrap_add(&bg, dist, status);
        added++;
    }
    bg_bootstrap_finalize(&bg);

    /* pass 2: process ALL data rows from the start */
    fseek(fp, pos_after_header, SEEK_SET);
    while (fgets(line, sizeof line, fp)) {
        long long seq = 0;
        int dist[BG_N_CELLS], status[BG_N_CELLS];
        if (parse_row(line, &seq, dist, status) < N_COLS_EXPECTED) {
            continue;
        }
        bool occ[BG_N_CELLS];
        float dev[BG_N_CELLS];
        bg_process(&bg, dist, status, occ, dev);

        int cells[BG_N_CELLS];
        int blob_n = det_largest_blob(occ, cells);

        crossing_event_t ev = tracker_update(&tk, cells, blob_n, dev, seq);
        if (ev.valid) {
            print_event(&ev);
        }
    }
    crossing_event_t ev = tracker_flush(&tk);
    if (ev.valid) {
        print_event(&ev);
    }

    fclose(fp);
    return 0;
}
