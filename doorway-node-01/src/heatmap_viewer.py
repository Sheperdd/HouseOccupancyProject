import sys
import numpy as np
import matplotlib.pyplot as plt
import serial

BAUD = 921600
GRID_SIZE = 8

DEPTH_MIN_MM = 200
DEPTH_MAX_MM = 2200

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

            if not line.startswith("FRAME,"):
                print(f"Unexpected line (not a frame): {line}")
                continue
            
            fields = line[6:].split(",")
            if len(fields) != 130:
                continue # malformed line
            
            statuses = np.array(fields[66:130], dtype=np.int32).reshape((GRID_SIZE, GRID_SIZE))
            distances = np.array(fields[2:66], dtype=np.float32).reshape((GRID_SIZE, GRID_SIZE))
            # Mask out invalid measurements
            valid = (statuses == 5) | (statuses == 9)
            distances[~valid] = np.nan

            im.set_data(distances)
            ax.set_title(f"VL53L5CX Live Depth Map - Frame {fields[0]}")
            fig.canvas.draw_idle()
            fig.canvas.flush_events()

    except KeyboardInterrupt:
            print("Exiting...")
