"""
tracker.py  --  Temporal layer: per-frame largest-blob centroids -> crossing events.

PORTABILITY CONTRACT (same as background.py)
--------------------------------------------
Plain Python only. No numpy/pandas. Fixed-size buffers, integer/float math
that maps 1:1 to C. The replay harness may use numpy; this file may not.

PIPELINE POSITION
-----------------
BackgroundModel.process() -> occupied cells + dev
   -> largest_blob()            (connectivity, this module)
   -> blob_centroid()           (dev-weighted, this module)
   -> Tracker.update()          (lifecycle + direction + event)

DIRECTION MODEL
---------------
Travel is DIAGONAL across the grid (Phase 1 finding #5). in/out is NOT a grid
axis -- it's projection of the track's net displacement onto a per-node
`in_axis` vector calibrated once at install (see calibrate_in_axis()).
"""

GRID = 8
N_CELLS = GRID * GRID

# ---- tuning knobs ---------------------------------------------------------
MIN_BLOB_CELLS = 4       # gate: track is "occupied" if largest blob >= this
GRACE_FRAMES = 3         # bridge brief gate dropouts without splitting a track
MAX_TRACK_FRAMES = 100   # force-close runaway tracks (also sets C buffer size)
MIN_NET_DISPLACEMENT = 1.2   # cells; below this, track is loiter/jitter -> no event
MIN_TRACK_FRAMES = 3     # reject ultra-short tracks (mostly redundant w/ gate+grace)
ENDPOINT_AVG = 3         # average first/last N centroids for robust endpoints
# ---------------------------------------------------------------------------


def rc_to_idx(row, col):
    return row * GRID + col


def idx_to_rc(i):
    return i // GRID, i % GRID


# ---- connectivity ---------------------------------------------------------
def largest_blob(occupied):
    """
    8-connected connected-components over the occupied cell indices.
    Returns the largest component as a list of indices (empty if none).

    8-connectivity (incl. diagonals) because travel is diagonal -- a person's
    blob can be diagonally linked; 4-conn would fragment it (finding #5).

    C-PORT NOTE: `occ_set` -> bool occ[64]; `seen` -> bool seen[64];
    `stack` -> fixed int stack[64]. No dynamic allocation needed.
    """
    if not occupied:
        return []
    occ_set = set(occupied)
    seen = set()
    best = []
    for start in occupied:
        if start in seen:
            continue
        # flood fill this component (iterative stack, not recursion)
        comp = []
        stack = [start]
        seen.add(start)
        while stack:
            cur = stack.pop()
            comp.append(cur)
            r, c = idx_to_rc(cur)
            for dr in (-1, 0, 1):
                for dc in (-1, 0, 1):
                    if dr == 0 and dc == 0:
                        continue
                    nr, nc = r + dr, c + dc
                    if 0 <= nr < GRID and 0 <= nc < GRID:
                        ni = rc_to_idx(nr, nc)
                        if ni in occ_set and ni not in seen:
                            seen.add(ni)
                            stack.append(ni)
        if len(comp) > len(best):
            best = comp
    return best


def blob_centroid(blob, dev):
    """
    Deviation-weighted centroid of a blob -> (cx, cy) in fractional grid coords.
    cx = column axis, cy = row axis. Weighting by dev tracks the closest point
    (head/shoulders, highest SNR) and damps fringe-cell flicker.
    Returns None for an empty blob.
    """
    if not blob:
        return None
    wsum = 0.0
    cxsum = 0.0
    cysum = 0.0
    for i in blob:
        w = dev[i]
        if w <= 0:
            continue
        r, c = idx_to_rc(i)
        wsum += w
        cxsum += w * c
        cysum += w * r
    if wsum <= 0:
        return None
    return (cxsum / wsum, cysum / wsum)


# ---- direction calibration -------------------------------------------------
def calibrate_in_axis(out_displacements):
    """
    Derive the per-node `in_axis` unit vector from KNOWN-OUT crossings.
    in_axis = -mean(out displacement vectors), normalized.
    out_displacements: list of (dx, dy). Returns (ax, ay) unit vector.
    """
    if not out_displacements:
        raise ValueError("need at least one known-out displacement")
    mx = sum(d[0] for d in out_displacements) / len(out_displacements)
    my = sum(d[1] for d in out_displacements) / len(out_displacements)
    # in is opposite of out
    ax, ay = -mx, -my
    mag = (ax * ax + ay * ay) ** 0.5
    if mag == 0:
        raise ValueError("degenerate in_axis (zero mean displacement)")
    return (ax / mag, ay / mag)


# ---- track -----------------------------------------------------------------
class _Track:
    """One continuous occupancy: ordered centroids + timing."""
    __slots__ = ("centroids", "sizes", "t_start", "t_end", "frames")

    def __init__(self, t):
        self.centroids = []   # list of (cx, cy)
        self.sizes = []       # largest-blob size per frame
        self.t_start = t
        self.t_end = t
        self.frames = 0

    def add(self, centroid, size, t):
        if centroid is not None:
            self.centroids.append(centroid)
            self.sizes.append(size)
        self.t_end = t
        self.frames += 1


class Tracker:
    """
    Feed it one frame at a time via update(). Emits a crossing event (dict)
    when a track closes and qualifies, else None.

    in_axis must be set before in/out labeling means anything. You can run the
    tracker with in_axis=None to COLLECT displacement vectors first (calibration
    pass), then set it and run again for scoring.
    """

    def __init__(self, node_id="doorway-node-01", in_axis=None):
        self.node_id = node_id
        self.in_axis = in_axis
        self._track = None
        self._grace = 0

    def update(self, largest_blob_cells, dev, t):
        """
        largest_blob_cells: output of largest_blob() for this frame
        dev: length-64 deviation array (for centroid weighting)
        t:   timestamp (CSV frame time in replay; uptime us on device pre-NTP)
        Returns an event dict, or None.
        """
        size = len(largest_blob_cells)
        occupied = size >= MIN_BLOB_CELLS

        if occupied:
            centroid = blob_centroid(largest_blob_cells, dev)
            if self._track is None:
                self._track = _Track(t)
            self._track.add(centroid, size, t)
            self._grace = 0
            # force-close runaway track
            if self._track.frames >= MAX_TRACK_FRAMES:
                return self._close()
            return None

        # not occupied this frame
        if self._track is not None:
            self._grace += 1
            self._track.t_end = t
            if self._grace > GRACE_FRAMES:
                return self._close()
        return None

    def _close(self):
        """Classify the finished track. Return event dict or None."""
        tr = self._track
        self._track = None
        self._grace = 0

        if tr is None or len(tr.centroids) < MIN_TRACK_FRAMES:
            return None

        # robust endpoints
        n = len(tr.centroids)
        k = min(ENDPOINT_AVG, n // 2)
        if k < 1:
            k = 1
        sx = sum(c[0] for c in tr.centroids[:k]) / k
        sy = sum(c[1] for c in tr.centroids[:k]) / k
        ex = sum(c[0] for c in tr.centroids[-k:]) / k
        ey = sum(c[1] for c in tr.centroids[-k:]) / k
        dx, dy = ex - sx, ey - sy
        net = (dx * dx + dy * dy) ** 0.5

        # path length (for straightness / confidence)
        path = 0.0
        for a, b in zip(tr.centroids, tr.centroids[1:]):
            path += ((b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2) ** 0.5

        # debounce: loiter-and-return or jitter -> no crossing
        if net < MIN_NET_DISPLACEMENT:
            return None

        # direction via projection onto in_axis
        if self.in_axis is None:
            direction = None          # calibration pass: caller reads (dx,dy)
            s = None
        else:
            s = dx * self.in_axis[0] + dy * self.in_axis[1]
            direction = "in" if s > 0 else "out"

        straightness = (net / path) if path > 0 else 0.0
        confidence = max(0.0, min(1.0, straightness))

        return {
            "node_id": self.node_id,
            "event": "crossing",
            "direction": direction,
            "t_start": tr.t_start,
            "t_end": tr.t_end,
            "frames": tr.frames,
            "displacement": (dx, dy),
            "net": net,
            "path": path,
            "confidence": round(confidence, 3),
            "peak_blob": max(tr.sizes) if tr.sizes else 0,
        }

    def flush(self):
        """Close any open track at end of stream."""
        if self._track is not None:
            return self._close()
        return None
