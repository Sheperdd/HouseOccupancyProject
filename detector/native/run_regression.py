"""
run_regression.py  --  Regression gate for the C detector port.

Runs the compiled native harness (replay/replay.exe) over every fixture and
compares its events to the Python golden files in golden/. Exact match is
required on direction, frame count, peak blob, and t_start; numeric fields
(dx, dy, net, confidence) must agree within tolerance (float32 C vs float64
Python produces tiny differences -- the handoff specifies tolerances, not
bit-equality).

Exit code 0 = all fixtures pass, 1 = any mismatch.

Usage:
    python run_regression.py            # builds nothing; expects ./replay(.exe)
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "..", "..", "fixtures")
GOLDEN = os.path.join(HERE, "golden")

# (fixture stem, expected event count) -- the acceptance criteria.
CASES = [
    ("empty_baseline", 0),
    ("walks_alternating_out_first", 8),
    ("fast_walks_alternating_out_first", 8),
    ("stop_in_doorway_out_first", 4),
]

POS_TOL = 0.05   # cells, for dx/dy/net
CONF_TOL = 0.05  # confidence units


def harness_path():
    for name in ("replay.exe", "replay"):
        p = os.path.join(HERE, name)
        if os.path.exists(p):
            return p
    sys.exit("harness not built: run `mingw32-make` (or `make`) first")


def parse_line(line):
    f = line.strip().split(",")
    if len(f) != 8:
        raise ValueError(f"bad event line: {line!r}")
    return {
        "dir": f[0],
        "frames": int(f[1]),
        "dx": float(f[2]),
        "dy": float(f[3]),
        "net": float(f[4]),
        "conf": float(f[5]),
        "peak": int(f[6]),
        "t_start": int(f[7]),
    }


def parse_events(text):
    return [parse_line(ln) for ln in text.splitlines() if ln.strip()]


def compare(golden, got):
    """Return list of human-readable diffs (empty == match)."""
    diffs = []
    if len(golden) != len(got):
        diffs.append(f"event count: golden={len(golden)} got={len(got)}")
        return diffs  # counts differ -> per-event compare is meaningless
    for i, (g, c) in enumerate(zip(golden, got)):
        for key in ("dir", "frames", "peak", "t_start"):
            if g[key] != c[key]:
                diffs.append(f"#{i} {key}: golden={g[key]} got={c[key]}")
        for key, tol in (("dx", POS_TOL), ("dy", POS_TOL),
                         ("net", POS_TOL), ("conf", CONF_TOL)):
            if abs(g[key] - c[key]) > tol:
                diffs.append(f"#{i} {key}: golden={g[key]:.3f} got={c[key]:.3f} "
                             f"(|d|={abs(g[key]-c[key]):.3f} > {tol})")
    return diffs


def main():
    exe = harness_path()
    all_ok = True
    for stem, expected_n in CASES:
        fixture = os.path.join(FIXTURES, stem + ".frames.csv")
        golden_file = os.path.join(GOLDEN, stem + ".txt")

        out = subprocess.run([exe, fixture], capture_output=True, text=True)
        if out.returncode != 0:
            print(f"FAIL  {stem}: harness exit {out.returncode}\n{out.stderr}")
            all_ok = False
            continue

        got = parse_events(out.stdout)
        with open(golden_file) as f:
            golden = parse_events(f.read())

        diffs = compare(golden, got)
        count_ok = (len(got) == expected_n)
        if not count_ok:
            diffs.insert(0, f"acceptance count: expected {expected_n}, got {len(got)}")

        if diffs:
            all_ok = False
            print(f"FAIL  {stem}  ({len(got)} events)")
            for d in diffs:
                print(f"        {d}")
        else:
            dirs = ",".join(e["dir"] for e in got)
            print(f"PASS  {stem:<34} {len(got)} events"
                  + (f"  [{dirs}]" if dirs else ""))

    print()
    if all_ok:
        print("ALL FIXTURES PASS  -- C port matches Python golden within tolerance")
        return 0
    print("REGRESSION FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
