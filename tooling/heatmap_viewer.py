import sys
import numpy as np
import matplotlib.pyplot as plt
import serial

BAUD = 921600
GRID_SIZE = 8

DEPTH_MIN_MM = 200
DEPTH_MAX_MM = 2200

SEQ, ESP_T_US = 0, 1
DIST_START, DIST_END = 2, 2 + 64
STATUS_START, STATUS_END = DIST_END, DIST_END + 64

def main():
    port = sys.argv[1]
    ser = serial.Serial(port, BAUD, timeout=1)

    grid = np.full((GRID_SIZE, GRID_SIZE), DEPTH_MAX_MM, dtype=np.int32)
    plt.ion()
    fig, ax = plt.subplots(figsize=(6, 6))
    im = ax.imshow(
        grid,
        cmap="inferno_r",
        vmin=DEPTH_MIN_MM,
        vmax=DEPTH_MAX_MM,
        interpolation="nearest",
    )
    plt.colorbar(im, ax=ax, label="Distance (mm)")
    ax.set_title("VL53L5CX Live Depth Map - Waiting for frames")
    plt.show(block=False)

    print(f"Reading from {port} at {BAUD} baud. Press Ctrl+C to stop.")
    try:
        while True:
            raw = ser.readline()
            if not raw:
                continue # timeout, no data received
            
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")

            if line.startswith(("FRAME,", "STATS,", "I (", "W (", "E (")):
                pass # These are expected lines from the ESP32. Ignore them if they don't match the frame format.
            else:
                print(f"Unexpected line (not a frame): {line}")
                continue
            
            fields = line[6:].split(",")
            if len(fields) != STATUS_END:
                continue # malformed line
            
            distances = np.array(fields[DIST_START:DIST_END], dtype=np.float32).reshape((GRID_SIZE, GRID_SIZE))
            statuses = np.array(fields[STATUS_START:STATUS_END], dtype=np.int32).reshape((GRID_SIZE, GRID_SIZE))
            unique, counts = np.unique(statuses, return_counts=True)
            # Mask out invalid measurements
            # Valid status codes:
            #    5 = range valid (cononical "good" reading)
            #    9 = range valid, large pulse (still good, just stronger reflection than expected )
            #   10 = range valid, no previous range used (also good, just no history to compare against)
            #  255 = No target detected (not an error, just means nothing was in range), but 
            valid = (statuses == 5) | (statuses == 9) | (statuses == 10) | (statuses == 255)
            distances[~valid] = np.nan

            im.set_data(distances)
            ax.set_title(f"VL53L5CX Live Depth Map - Frame {fields[0]}")
            fig.canvas.draw_idle()
            fig.canvas.flush_events()

    except KeyboardInterrupt:
            print("Exiting...")

if __name__ == "__main__":
    main()
 
