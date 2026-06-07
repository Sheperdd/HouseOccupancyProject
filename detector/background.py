"""
background.py  --  Per-cell background model for the VL53L5CX doorway node.

PORTABILITY CONTRACT
--------------------
This module is the GOLDEN REFERENCE that gets ported to C/ESP-IDF in a later
phase. Keep it that way:
  * plain Python only -- NO numpy, NO pandas, NO list comprehensions that
    hide loops you'd have to unroll in C.
  * fixed-size arrays of length N_CELLS. No dynamic growth in the hot path.
  * integer/float math that maps 1:1 to C.
The harness (replay_background.py) is allowed to use numpy/pandas. This file
is not. If you reach for numpy here, stop -- it belongs in the harness.

DATA MODEL
----------
A frame = two arrays of length 64:
    dist[i]   -- distance in mm for cell i        (int)
    status[i] -- ST range status code for cell i  (int)
Trusted status codes are {5, 9, 10, 255} (Phase 1 finding #1).
Cell HOT_PIXEL is masked unconditionally (Phase 1 finding #4).
"""

GRID = 8
N_CELLS = GRID * GRID          # 64

VALID_STATUS = (5, 9, 10, 255)  # Phase 1 finding #1
HOT_PIXELS = (5,)               # flat index of cell (row=0,col=5) -> 0*8+5 = 5
                                # kept as DATA, not an `if` -- swap to per-node
                                # auto-detection later by changing this tuple.

# ---- tuning knobs (you will sweep these against the fixtures) -------------
BOOTSTRAP_FRAMES = 50    # frames assumed empty at startup to seed bg
ALPHA = 0.01             # EMA learning rate; 1/ALPHA ~= frames to adapt
                         #   0.01 -> ~100 frames -> ~10s at 10Hz
K_OCCUPY = 6.0           # occupied if dev > K_OCCUPY * mad   (declare)
K_CLEAR  = 4.0           # clears  if dev < K_CLEAR  * mad   (hysteresis)
MAD_FLOOR_MM = 15.0      # min noise floor so quiet cells aren't hair-trigger
MIN_OCCUPIED_CELLS = 2   # frame is "occupied" if >= this many cells fire
# ---------------------------------------------------------------------------


def rc_to_idx(row, col):
    """Map (row, col) -> flat index. Keep one convention everywhere."""
    return row * GRID + col


def _trusted(status, idx):
    """Is cell idx usable this frame?"""
    if idx in HOT_PIXELS:
        return False
    return status[idx] in VALID_STATUS


def _median(values):
    """Median of a list. Plain, no numpy. values must be non-empty."""
    s = sorted(values)
    n = len(s)
    mid = n // 2
    if n % 2 == 1:
        return float(s[mid])
    return 0.5 * (s[mid - 1] + s[mid])


class BackgroundModel:
    """
    Holds per-cell background estimate and noise estimate.
    Lifecycle:
        m = BackgroundModel()
        for first BOOTSTRAP_FRAMES frames:   m.bootstrap_add(dist, status)
        m.bootstrap_finalize()
        then per frame:                      occ, dev = m.process(dist, status)
    """

    def __init__(self):
        self.bg = [0.0] * N_CELLS          # background estimate (mm)
        self.mad = [0.0] * N_CELLS         # per-cell noise estimate (mm)
        self.calibrated = [False] * N_CELLS
        self._boot = [[] for _ in range(N_CELLS)]  # bootstrap sample buffers
        self._ready = False
        # per-cell hysteresis latch: is this cell currently "occupied"?
        self._cell_hot = [False] * N_CELLS

    # ---- bootstrap ---------------------------------------------------------
    def bootstrap_add(self, dist, status):
        """Collect one frame's trusted samples into per-cell buffers."""
        for i in range(N_CELLS):
            if _trusted(status, i):
                self._boot[i].append(dist[i])

    def bootstrap_finalize(self):
        """
        Turn collected samples into bg (median) + mad (median abs deviation).
        Median, not mean: tolerates a person wandering through during calib.
        A cell with no trusted samples stays uncalibrated (handled as no-data).
        """
        for i in range(N_CELLS):
            samples = self._boot[i]
            if len(samples) == 0:
                self.calibrated[i] = False
                continue
            med = _median(samples)
            abs_dev = [abs(v - med) for v in samples]
            mad = _median(abs_dev)
            self.bg[i] = med
            self.mad[i] = max(mad, MAD_FLOOR_MM)
            self.calibrated[i] = True
        self._boot = [[] for _ in range(N_CELLS)]  # free memory
        self._ready = True

    # ---- per-frame ---------------------------------------------------------
    def process(self, dist, status):
        """
        Returns (occupied_cells, dev) where:
          occupied_cells -- list of flat indices currently occupied
          dev            -- length-64 list, deviation in mm (0.0 where no-data)
        Side effect: gated EMA update of bg on frames deemed empty.
        """
        if not self._ready:
            raise RuntimeError("call bootstrap_finalize() first")

        dev = [0.0] * N_CELLS
        occupied = []

        for i in range(N_CELLS):
            if not self.calibrated[i] or not _trusted(status, i):
                self._cell_hot[i] = False
                continue
            d = self.bg[i] - dist[i]      # positive = closer than bg = object
            dev[i] = d
            hi = K_OCCUPY * self.mad[i]
            lo = K_CLEAR * self.mad[i]
            # hysteresis: high bar to turn on, low bar to turn off
            if self._cell_hot[i]:
                if d < lo:
                    self._cell_hot[i] = False
            else:
                if d > hi:
                    self._cell_hot[i] = True
            if self._cell_hot[i]:
                occupied.append(i)

        frame_occupied = len(occupied) >= MIN_OCCUPIED_CELLS

        # GATE: only learn background from frames that look empty.
        # This is the fix for absorbing a stationary person (stop-in-doorway).
        if not frame_occupied:
            for i in range(N_CELLS):
                if self.calibrated[i] and _trusted(status, i):
                    self.bg[i] = (1.0 - ALPHA) * self.bg[i] + ALPHA * dist[i]

        return occupied, dev
