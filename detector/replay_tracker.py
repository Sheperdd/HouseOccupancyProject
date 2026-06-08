"""
replay_tracker.py  --  Full detection chain over a fixture, with scoring.

HARNESS code: numpy/pandas allowed.

Two-pass design:
  Pass 1 (calibration): run the chain over walks_alternating_out_first with
    in_axis=None, collect displacement vectors, label them with the KNOWN
    alternating truth (out,in,out,in,...), average the OUT vectors -> in_axis.
  Pass 2 (scoring): run the chain over a target fixture with that in_axis,
    compare emitted events to the truth sequence (count + direction).

Usage:
    # calibrate on walks, score on fast walks (held-out, honest):
    python replay_tracker.py \
        --calib fixtures/walks_alternating_out_first.frames.csv \
        --score fixtures/fast_walks_alternating_out_first.frames.csv \
        --truth out,in,out,in,out,in,out,in

    # score the stop fixture (set truth to your actual rep pattern):
    python replay_tracker.py \
        --calib fixtures/walks_alternating_out_first.frames.csv \
        --score fixtures/stop_in_doorway_out_first.frames.csv \
        --truth out,in,out,in,out,in
"""
import sys
import argparse
import pandas as pd

from background import BackgroundModel, N_CELLS, BOOTSTRAP_FRAMES
from tracker import (
    Tracker, largest_blob, calibrate_in_axis, GRID,
    MIN_BLOB_CELLS, GRACE_FRAMES, MIN_NET_DISPLACEMENT,
)


def find_columns(df):
    cols = list(df.columns)
    for dpfx, spfx in (("d", "s"), ("dist", "status"), ("dist_", "status_")):
        dcols = [c for c in cols if str(c).lower().startswith(dpfx)
                 and str(c).lower().replace(dpfx, "").isdigit()]
        scols = [c for c in cols if str(c).lower().startswith(spfx)
                 and str(c).lower().replace(spfx, "").isdigit()]
        if len(dcols) == N_CELLS and len(scols) == N_CELLS:
            dcols.sort(key=lambda c: int(str(c).lower().replace(dpfx, "")))
            scols.sort(key=lambda c: int(str(c).lower().replace(spfx, "")))
            return dcols, scols
    if len(cols) >= 2 + 2 * N_CELLS:
        return cols[2:2 + N_CELLS], cols[2 + N_CELLS:2 + 2 * N_CELLS]
    raise SystemExit(f"can't locate dist/status columns in {cols[:6]}")


def run_chain(path, in_axis):
    """Run background->blob->tracker over a fixture. Returns list of events."""
    df = pd.read_csv(path)
    dcols, scols = find_columns(df)
    n = len(df)

    bg = BackgroundModel()
    for r in range(min(BOOTSTRAP_FRAMES, n)):
        bg.bootstrap_add([int(df.iloc[r][c]) for c in dcols],
                         [int(df.iloc[r][c]) for c in scols])
    bg.bootstrap_finalize()

    tr = Tracker(in_axis=in_axis)
    events = []
    for r in range(n):
        dist = [int(df.iloc[r][c]) for c in dcols]
        status = [int(df.iloc[r][c]) for c in scols]
        occ, dev = bg.process(dist, status)
        blob = largest_blob(occ)
        # use frame timestamp if present (col index 1), else row number
        t = float(df.iloc[r, 1]) if df.shape[1] > 1 else r
        ev = tr.update(blob, dev, t)
        if ev:
            events.append(ev)
    ev = tr.flush()
    if ev:
        events.append(ev)
    return events, n


def calibrate(path, truth):
    """Pass 1: collect OUT displacements, derive in_axis."""
    events, n = run_chain(path, in_axis=None)
    print(f"[calib] {path}: {n} frames, {len(events)} tracks emitted "
          f"(truth has {len(truth)})")
    if len(events) != len(truth):
        print(f"[calib] WARNING: track count {len(events)} != truth {len(truth)}; "
              f"in_axis derivation may be misaligned. Inspect below.")
    out_disps = []
    for ev, label in zip(events, truth):
        d = ev["displacement"]
        print(f"  track frames={ev['frames']:>3} disp=({d[0]:+.2f},{d[1]:+.2f}) "
              f"net={ev['net']:.2f} peak={ev['peak_blob']:>2} truth={label}")
        if label == "out":
            out_disps.append(d)
    axis = calibrate_in_axis(out_disps)
    print(f"[calib] in_axis = ({axis[0]:+.3f}, {axis[1]:+.3f})  "
          f"from {len(out_disps)} out-crossings")
    return axis


def score(path, truth, in_axis):
    """Pass 2: emit events with in_axis, compare to truth."""
    events, n = run_chain(path, in_axis=in_axis)
    print(f"\n[score] {path}: {n} frames")
    print(f"[score] emitted {len(events)} events, truth expects {len(truth)}")
    print("\n  emitted events:")
    for i, ev in enumerate(events):
        d = ev["displacement"]
        print(f"    #{i:<2} dir={str(ev['direction']):>3} "
              f"frames={ev['frames']:>3} disp=({d[0]:+.2f},{d[1]:+.2f}) "
              f"net={ev['net']:.2f} conf={ev['confidence']:.2f} "
              f"peak={ev['peak_blob']:>2} "
              f"t_start={ev['t_start']:.2f}")

    # count accuracy
    print(f"\n  COUNT: emitted {len(events)} vs expected {len(truth)}  "
          f"({'OK' if len(events) == len(truth) else 'MISMATCH'})")

    # direction accuracy on the overlap (only meaningful if counts align;
    # if they don't, this is a best-effort positional compare)
    m = min(len(events), len(truth))
    correct = sum(1 for i in range(m) if events[i]["direction"] == truth[i])
    if m:
        print(f"  DIRECTION: {correct}/{m} correct "
              f"({100*correct/m:.0f}%) on positional overlap")
        if len(events) != len(truth):
            print("  (counts differ -- positional compare is approximate; "
                  "fix count first)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--calib", required=True)
    ap.add_argument("--score", required=True)
    ap.add_argument("--truth", required=True,
                    help="comma list e.g. out,in,out,in")
    ap.add_argument("--calib-truth", default=None,
                    help="truth for the calib fixture if different "
                         "(default: alternating out,in matching its length)")
    args = ap.parse_args()

    score_truth = [s.strip() for s in args.truth.split(",")]

    # build calib truth: default alternating out-first, length inferred by a
    # dry run unless given explicitly.
    if args.calib_truth:
        calib_truth = [s.strip() for s in args.calib_truth.split(",")]
    else:
        dry, _ = run_chain(args.calib, in_axis=None)
        calib_truth = ["out" if i % 2 == 0 else "in" for i in range(len(dry))]
        print(f"[calib] inferred alternating truth of length {len(calib_truth)} "
              f"(out-first). Override with --calib-truth if wrong.")

    axis = calibrate(args.calib, calib_truth)
    score(args.score, score_truth, axis)


if __name__ == "__main__":
    main()
