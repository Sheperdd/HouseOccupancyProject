"""
replay_background.py  --  Replay a captured .frames.csv against BackgroundModel.

This is HARNESS code: numpy/pandas are fine here. Never ported to C.

Usage:
    python replay_background.py empty_baseline.frames.csv
    python replay_background.py walks_alternating_out_first.frames.csv

What it checks:
  * On empty_baseline: occupied-frame count should be ~0. Any occupied frame
    on a known-empty scene is a FALSE POSITIVE -> tighten K_OCCUPY or check
    for an un-masked stuck cell.
  * Per-cell "how often did this cell fire" table: reveals stuck/hot cells you
    haven't masked yet (besides the known (0,5)).
  * Background drift: how far each cell's bg moved from bootstrap to end ->
    confirms the EMA is tracking mount drift (Phase 1 finding #3).

COLUMN LAYOUT ASSUMPTION
------------------------
Expects 64 distance columns and 64 status columns. It auto-detects by name if
your headers look like d0..d63 / s0..s63 (or dist_*/status_*). If detection
fails it falls back to POSITION: first 2 cols = seq,timestamp; next 64 = dist;
next 64 = status. If your capture.py wrote a different layout, fix DIST_COLS /
STAT_COLS below -- this is the only spot that knows the file format.
"""
import sys
import pandas as pd

from background import (
    BackgroundModel, N_CELLS, GRID, BOOTSTRAP_FRAMES, rc_to_idx, HOT_PIXELS,
    MIN_BLOB_CELLS, label_blobs,
)


def find_columns(df):
    cols = list(df.columns)
    # try name-based detection
    for dpfx, spfx in (("d", "s"), ("dist", "status"), ("dist_", "status_")):
        dcols = [c for c in cols if str(c).lower().startswith(dpfx)
                 and str(c).lower().replace(dpfx, "").isdigit()]
        scols = [c for c in cols if str(c).lower().startswith(spfx)
                 and str(c).lower().replace(spfx, "").isdigit()]
        if len(dcols) == N_CELLS and len(scols) == N_CELLS:
            dcols.sort(key=lambda c: int(str(c).lower().replace(dpfx, "")))
            scols.sort(key=lambda c: int(str(c).lower().replace(spfx, "")))
            return dcols, scols
    # fall back to positional: [meta, meta, 64 dist, 64 status]
    if len(cols) >= 2 + 2 * N_CELLS:
        return cols[2:2 + N_CELLS], cols[2 + N_CELLS:2 + 2 * N_CELLS]
    raise SystemExit(f"can't locate 64 dist + 64 status columns in {cols[:6]}...")


def main(path):
    df = pd.read_csv(path)
    dcols, scols = find_columns(df)
    n = len(df)
    print(f"{path}: {n} frames, {len(df.columns)} columns")
    print(f"  dist cols: {dcols[0]}..{dcols[-1]}   stat cols: {scols[0]}..{scols[-1]}")

    m = BackgroundModel()

    # bootstrap on the first BOOTSTRAP_FRAMES rows
    for r in range(min(BOOTSTRAP_FRAMES, n)):
        dist = [int(df.iloc[r][c]) for c in dcols]
        status = [int(df.iloc[r][c]) for c in scols]
        m.bootstrap_add(dist, status)
    m.bootstrap_finalize()

    bg_start = list(m.bg)
    cell_fire_count = [0] * N_CELLS
    occupied_frames_any = 0      # frames with >=1 cell fired
    occupied_frames_gate = 0     # frames the GATE calls occupied (blob >= MIN)
    blob_hist = {}               # how many frames had largest-blob-size == k

    for r in range(n):
        dist = [int(df.iloc[r][c]) for c in dcols]
        status = [int(df.iloc[r][c]) for c in scols]
        occ, dev = m.process(dist, status)
        # largest connected blob this frame -- mirrors the gate in process()
        blobs = label_blobs(occ)
        largest = 0
        for b in blobs:
            if len(b) > largest:
                largest = len(b)
        blob_hist[largest] = blob_hist.get(largest, 0) + 1
        if len(occ) >= 1:
            occupied_frames_any += 1
            for i in occ:
                cell_fire_count[i] += 1
        if largest >= MIN_BLOB_CELLS:
            occupied_frames_gate += 1

    # ---- report ----
    print(f"\n  frames with >=1 occupied cell: {occupied_frames_any} / {n} "
          f"({100*occupied_frames_any/n:.1f}%)")
    print(f"  frames the GATE calls occupied (largest blob >= {MIN_BLOB_CELLS} cells): "
          f"{occupied_frames_gate} / {n} ({100*occupied_frames_gate/n:.1f}%)")
    print("  (on empty_baseline the GATE number is what must be ~0)")

    print("\n  largest-blob-size histogram (k = biggest connected blob in a "
          "frame -> how many frames):")
    print("  (on empty_baseline this must pile at 0-1; any connected blob >= "
          f"{MIN_BLOB_CELLS} is a surprise to investigate)")
    for k in sorted(blob_hist):
        bar = "#" * min(60, blob_hist[k])
        print(f"    {k:>2} cells: {blob_hist[k]:>4}  {bar}")

    print("\n  per-cell fire count (rows x cols, '.' = never fired):")
    print("        " + " ".join(f"c{c}" for c in range(GRID)))
    for row in range(GRID):
        cells = []
        for col in range(GRID):
            i = rc_to_idx(row, col)
            v = cell_fire_count[i]
            tag = "HOT" if i in HOT_PIXELS else (str(v) if v else ".")
            cells.append(f"{tag:>3}")
        print(f"    r{row}  " + " ".join(cells))

    print("\n  background drift bootstrap->end (mm), top 5 movers:")
    drift = sorted(
        ((abs(m.bg[i] - bg_start[i]), i) for i in range(N_CELLS) if m.calibrated[i]),
        reverse=True,
    )[:5]
    for d, i in drift:
        print(f"    cell ({i//GRID},{i%GRID}): {bg_start[i]:.0f} -> {m.bg[i]:.0f}  (d={d:.0f})")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: python replay_background.py <name>.frames.csv")
    main(sys.argv[1])
