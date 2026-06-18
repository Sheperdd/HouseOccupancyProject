"""Pure occupancy model — no IO, fully unit-testable.

Counts are kept per ZONE (see floorplan.py): a move transfers one person
from a source zone to a destination zone. The "outside" pseudo-zone absorbs
people silently. Decrements clamp at 0 but are COUNTED (clamp_count): a
clamp means reality and the model disagreed, which is exactly the drift
signal the Phase 4 backend work needs — never swallow it silently.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from .floorplan import OUTSIDE


@dataclass
class OccupancyState:
    counts: dict[str, int] = field(default_factory=dict)
    clamp_count: int = 0

    @property
    def house_total(self) -> int:
        # Recomputed, never tracked separately: one source of truth.
        return sum(self.counts.values())


def apply_move(state: OccupancyState, src: str, dest: str) -> dict[str, int]:
    """Move one person src -> dest; returns {zone: new_absolute_count} for
    changed zones. A same-zone move (crossing an unsensored-internal
    doorway's zone) is a no-op by construction."""
    changes: dict[str, int] = {}
    if src == dest:
        return changes
    if dest != OUTSIDE:
        state.counts[dest] = state.counts.get(dest, 0) + 1
        changes[dest] = state.counts[dest]
    if src != OUTSIDE:
        current = state.counts.get(src, 0)
        if current > 0:
            state.counts[src] = current - 1
        else:
            state.clamp_count += 1
        changes[src] = state.counts.get(src, 0)
    return changes


def reset(state: OccupancyState, counts: dict[str, int] | None = None) -> dict[str, int]:
    """Zero all zones, or set explicit counts. Returns the full new counts."""
    for zone in state.counts:
        state.counts[zone] = 0
    if counts:
        for zone, n in counts.items():
            if zone not in state.counts:
                raise KeyError(f"unknown zone {zone!r}")
            if n < 0:
                raise ValueError(f"count for {zone!r} must be >= 0")
            state.counts[zone] = n
    return dict(state.counts)
