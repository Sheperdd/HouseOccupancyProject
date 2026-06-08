"""
gen_golden.py  --  Emit the Python detector's events in a normalized, one-line-
per-event format so the C native harness output can be diffed against it.

This is the GOLDEN-REFERENCE generator for the C port. It runs the exact same
background.py + tracker.py cores the device firmware is ported from, with the
SAME hardcoded in_axis the C uses (so direction labels line up bit-for-bit in
intent). Output format, one line per emitted crossing:

    <dir>,<frames>,<dx>,<dy>,<net>,<conf>,<peak>,<t_start>

  dir      out|in
  frames   track length in frames (lifecycle count, incl. grace)
  dx,dy    net displacement components (cells), 3 dp
  net      |displacement| (cells), 3 dp
  conf     confidence 0..1, 3 dp
  peak     peak largest-blob size over the track
  t_start  track start timestamp (the fixture 'seq' column, matches device feed)

Usage:
    python gen_golden.py ../../fixtures/walks_alternating_out_first.frames.csv
    # writes golden/walks_alternating_out_first.txt  (and prints to stdout)

HARNESS code: pandas allowed (never ported to C).
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import pandas as pd  # noqa: E402

from background import BackgroundModel, N_CELLS, BOOTSTRAP_FRAMES  # noqa: E402
from tracker import Tracker, largest_blob  # noqa: E402

# Must match the C tracker.h IN_AXIS_X / IN_AXIS_Y exactly.
IN_AXIS = (-0.456, -0.890)


def find_columns(df):
    """Positional layout: host_ts, seq, esp_t_us, then 64 dist, then 64 status."""
    cols = list(df.columns)
    if len(cols) >= 3 + 2 * N_CELLS:
        return cols[3:3 + N_CELLS], cols[3 + N_CELLS:3 + 2 * N_CELLS], cols
    raise SystemExit(f"unexpected column count {len(cols)} in {cols[:6]}")


def run(path):
    df = pd.read_csv(path)
    dcols, scols, cols = find_columns(df)
    seq_col = cols[1]  # 'seq' -- the timestamp the tracker records as t_start
    n = len(df)

    bg = BackgroundModel()
    for r in range(min(BOOTSTRAP_FRAMES, n)):
        bg.bootstrap_add([int(df.iloc[r][c]) for c in dcols],
                         [int(df.iloc[r][c]) for c in scols])
    bg.bootstrap_finalize()

    tr = Tracker(in_axis=IN_AXIS)
    events = []
    for r in range(n):
        dist = [int(df.iloc[r][c]) for c in dcols]
        status = [int(df.iloc[r][c]) for c in scols]
        occ, dev = bg.process(dist, status)
        blob = largest_blob(occ)
        t = float(df.iloc[r][seq_col])
        ev = tr.update(blob, dev, t)
        if ev:
            events.append(ev)
    ev = tr.flush()
    if ev:
        events.append(ev)
    return events


def fmt(ev):
    dx, dy = ev["displacement"]
    return (f"{ev['direction']},{ev['frames']},{dx:.3f},{dy:.3f},"
            f"{ev['net']:.3f},{ev['confidence']:.3f},{ev['peak_blob']},"
            f"{ev['t_start']:.0f}")


def main(path):
    events = run(path)
    lines = [fmt(e) for e in events]
    out = "\n".join(lines) + ("\n" if lines else "")

    golden_dir = os.path.join(os.path.dirname(__file__), "golden")
    os.makedirs(golden_dir, exist_ok=True)
    name = os.path.basename(path).replace(".frames.csv", "").replace(".csv", "")
    dst = os.path.join(golden_dir, name + ".txt")
    with open(dst, "w", newline="\n") as f:
        f.write(out)

    sys.stdout.write(out)
    sys.stderr.write(f"[gen_golden] {len(events)} events -> {dst}\n")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: python gen_golden.py <fixture>.frames.csv")
    main(sys.argv[1])
