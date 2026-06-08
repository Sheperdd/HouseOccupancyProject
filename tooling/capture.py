#!/usr/bin/env python3
"""
VL53L5CX sensor capture script.
 
Reads structured FRAME and STATS lines from the ESP32's USB serial output
and writes them to two CSV files for later analysis. Designed to be run
during walk-through testing — start it, walk through the doorway a bunch
of times, Ctrl+C.
 
Usage:
    python capture.py <serial_port> <output_basename>
 
Example (Windows):
    python capture.py COM5 walk_in_slow_01
 
Produces:
    walk_in_slow_01.frames.csv   — one row per depth frame
    walk_in_slow_01.stats.csv    — one row per ESP32 STATS report (~1/sec)
 
Anything the ESP32 prints that isn't a FRAME or STATS line (boot logs,
ESP_LOG output) is echoed to stderr so it doesn't pollute the data files
but you can still see what the chip is saying.
"""
 
import argparse
import sys
import time
from pathlib import Path
 
import serial  # pyserial
 
 
BAUD = 921600
 
 
def make_frame_header():
    """
    Frame CSV columns:
      host_ts    — Unix time when this script received the line (s, µs precision)
      seq        — ESP32's monotonic frame counter (resets on chip reboot)
      esp_t_us   — esp_timer_get_time() at frame capture (µs since ESP32 boot)
      d00..d63   — distance_mm for the 64 zones, row-major
      s00..s63   — target_status for each zone (5 = valid, 9 = valid-large-pulse,
                   others = invalid for various reasons; see VL53L5CX user manual)
 
    The 2-digit zero-padded column names make `df[[f"d{i:02d}" for i in range(64)]]`
    work cleanly in pandas, and they sort lexicographically in the same order
    they appear in the grid.
    """
    cols = ["host_ts", "seq", "esp_t_us"]
    cols += [f"d{i:02d}" for i in range(64)]
    cols += [f"s{i:02d}" for i in range(64)]
    return ",".join(cols)
 
 
STATS_HEADER = ",".join([
    "host_ts",
    "esp_t_us",
    "free_heap",
    "min_free_heap",
    "stack_hwm",
    "avg_period_us",
    "max_period_us",
    "avg_read_us",
])
 
 
def parse_args():
    p = argparse.ArgumentParser(
        description="Capture VL53L5CX frame data from an ESP32 over USB serial."
    )
    p.add_argument("port", help="Serial port (e.g. COM5 on Windows, /dev/ttyUSB0 on Linux)")
    p.add_argument("basename", help="Output file basename (no extension)")
    p.add_argument("--force", action="store_true",
                   help="Overwrite existing output files instead of refusing to start")
    return p.parse_args()
 
 
def main():
    args = parse_args()
 
    frames_path = Path(f"fixtures/{args.basename}.frames.csv")
    stats_path = Path(f"fixtures/{args.basename}.stats.csv")
 
    if not args.force:
        for p in (frames_path, stats_path):
            if p.exists():
                sys.exit(f"Refusing to overwrite existing file: {p} (use --force)")
 
    # buffering=1 → line buffering. Each '\n' flushes to disk immediately,
    # so a Ctrl+C or crash doesn't lose the last few seconds of capture.
    with serial.Serial(args.port, BAUD, timeout=1) as ser, \
         open(frames_path, "w", buffering=1) as frames_f, \
         open(stats_path, "w", buffering=1) as stats_f:
 
        frames_f.write(make_frame_header() + "\n")
        stats_f.write(STATS_HEADER + "\n")
 
        print(f"Capturing from {args.port} at {BAUD} baud.")
        print(f"  Frames -> {frames_path}")
        print(f"  Stats  -> {stats_path}")
        print("Press Ctrl+C to stop.\n")
 
        frame_count = 0
        ignored = 0
 
        try:
            while True:
                # readline() returns b"" on the 1-second timeout. That's fine —
                # we loop and try again. The timeout is what makes Ctrl+C
                # responsive even when no data is flowing.
                raw = ser.readline()
                if not raw:
                    continue
 
                # Capture host time the instant we read the line. We do this
                # before any parsing so the timestamp reflects when the data
                # arrived, not when we got around to processing it.
                host_ts = time.time()
                line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
 
                if line.startswith("FRAME,"):
                    # After "FRAME," we expect: seq, esp_t_us, 64 distances,
                    # 64 statuses = 130 fields. A wrong count means the line
                    # was truncated (USB hiccup) or malformed.
                    fields = line[6:].split(",")
                    if len(fields) != 1 + 1 + 64 + 64:
                        ignored += 1
                        continue
                    frames_f.write(f"{host_ts:.6f},{','.join(fields)}\n")
                    frame_count += 1
 
                elif line.startswith("STATS,"):
                    # After "STATS,": esp_t_us, free_heap, min_heap, stack_hwm,
                    # avg_period, max_period, avg_read = 7 fields.
                    fields = line[6:].split(",")
                    if len(fields) != 7:
                        ignored += 1
                        continue
                    stats_f.write(f"{host_ts:.6f},{','.join(fields)}\n")
 
                    # STATS arrives ~once/sec, so we use it as our on-screen
                    # heartbeat. If you stop seeing these scroll past, the
                    # ESP32 has gone silent and you should investigate.
                    try:
                        _, free_heap, _min, stack_hwm, avg_period, max_period, avg_read = fields
                        period_us = int(avg_period)
                        rate = 1_000_000 / period_us if period_us > 0 else 0
                        print(
                            f"[{time.strftime('%H:%M:%S')}]  "
                            f"frames={frame_count:>5}  "
                            f"rate={rate:5.2f} Hz  "
                            f"read={int(avg_read)/1000:5.2f} ms  "
                            f"max_period={int(max_period)/1000:6.2f} ms  "
                            f"heap={int(free_heap)//1024} KB  "
                            f"stack={int(stack_hwm)} B"
                        )
                    except (ValueError, ZeroDivisionError):
                        pass  # malformed STATS — file write already happened, just skip the print
 
                else:
                    # Boot logs, ESP_LOGI lines, sensor init messages, anything
                    # not prefixed FRAME/STATS. Goes to stderr so it shows up
                    # in your terminal but doesn't get mixed into the CSV files.
                    print(f"  | {line}", file=sys.stderr)
 
        except KeyboardInterrupt:
            print(f"\nStopped. Captured {frame_count} frames "
                  f"({ignored} malformed lines ignored).")
 
 
if __name__ == "__main__":
    main()
 
