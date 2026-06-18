"""Event pipeline — the spine of the backend.

Every MQTT event runs the same fixed stages, in order:

    parse/normalize -> dedup -> correlate (Phase 4 seam) -> occupancy
                    -> persist -> broadcast

Keeping the stages explicit (rather than scattered across handlers) is the
point: when Phase 4 adds cross-node correlation and out-of-order handling,
they slot into stages that already exist.
"""

from __future__ import annotations

import json
import logging
import time
from collections import deque

from . import occupancy
from .floorplan import Floorplan
from .ws import Hub

log = logging.getLogger(__name__)

RECENT_EVENTS = 50  # event-log entries kept for the connect snapshot


def now_ms() -> int:
    return int(time.time() * 1000)


class DedupWindow:
    """Ring of recently seen (seq, t_us) pairs for ONE node.

    QoS1 + the firmware's PUBACK-timeout re-publish means duplicates are
    expected, not exceptional. Key is (seq, t_us), not seq alone: a node
    reboot restarts seq near 0, but t_us (boot-relative µs) differs, so
    post-reboot events never false-positive against pre-reboot ones.
    """

    def __init__(self, size: int = 64) -> None:
        self._ring: deque[tuple[int, int]] = deque(maxlen=size)

    def seen(self, seq: int, t_us: int) -> bool:
        key = (seq, t_us)
        if key in self._ring:  # 64 entries; linear scan is fine
            return True
        self._ring.append(key)
        return False


def correlate(event: dict) -> dict | None:
    """Phase 4 seam: cross-node correlation.

    Future contract: may suppress an event (return None) when it is the
    second half of a hallway pair (exit room A + enter room B within a short
    window = ONE person moving, not two events), may rewrite it into a merged
    A->B move, and may delay events to reorder by timestamp. For Phase 5 it
    is a pass-through.
    """
    return event


class Pipeline:
    def __init__(self, floorplan: Floorplan, hub: Hub, db=None) -> None:
        self.floorplan = floorplan
        self.hub = hub
        self.db = db  # None until M3 wires SQLite in
        self.state = occupancy.OccupancyState({z: 0 for z in floorplan.zone_ids()})
        self._dedup: dict[str, DedupWindow] = {}
        # Node registry builds itself from retained status messages — a node
        # the backend has never heard of is the NORMAL startup path, not an
        # error (broker replays retained status at subscribe time).
        self.nodes: dict[str, dict] = {}
        self.recent_events: deque[dict] = deque(maxlen=RECENT_EVENTS)

    # ----- events ---------------------------------------------------------

    async def handle_event(self, payload: bytes | str, arrival_ms: int | None = None) -> None:
        arrival_ms = arrival_ms if arrival_ms is not None else now_ms()

        # Stage 1: parse/normalize
        try:
            raw = json.loads(payload)
            node_id = str(raw["node_id"])
            seq = int(raw["seq"])
            t_us = int(raw["t_us"])
            direction = raw["direction"]
            if direction not in ("in", "out"):
                raise ValueError(f"bad direction {direction!r}")
            t_unix_ms = int(raw.get("t_unix_ms", 0))
        except (ValueError, KeyError, TypeError) as exc:
            log.warning("dropping malformed event %r: %s", payload, exc)
            return
        # t_unix_ms == 0 is the firmware's "clock was not NTP-synced when the
        # crossing fired" sentinel — substitute arrival time and flag it.
        clock_synced = t_unix_ms != 0
        event_ts_ms = t_unix_ms if clock_synced else arrival_ms

        # Stage 2: dedup (per node)
        duplicate = self._dedup.setdefault(node_id, DedupWindow()).seen(seq, t_us)

        # Stage 3: correlation seam (pass-through in Phase 5)
        suppressed = correlate(raw) is None

        # Stage 4: map node -> doorway
        doorway = self.floorplan.doorway_for_node(node_id)
        if doorway is None:
            log.warning("event from node %s with no doorway in floorplan.json", node_id)

        entry = {
            "node_id": node_id,
            "seq": seq,
            "direction": direction,
            "event_ts_ms": event_ts_ms,
            "arrival_ts_ms": arrival_ms,
            "clock_synced": clock_synced,
            "net": raw.get("net"),
            "confidence": raw.get("confidence"),
            "peak_blob": raw.get("peak_blob"),
            "duplicate": duplicate,
            "unmapped": doorway is None,
            "doorway_id": doorway.id if doorway else None,
        }

        # Stage 5: persist (M3 — db lands with the REST milestone)
        if self.db is not None:
            self.db.insert_event(entry, raw)

        # Stage 6: broadcast. The log line goes out for EVERY event —
        # duplicates and unmapped included — so the dashboard shows traffic
        # even when counts don't move.
        self.recent_events.append(entry)
        await self.hub.broadcast({"type": "event_log_append", "event": entry})

        if duplicate or suppressed or doorway is None:
            return
        src_zone, dest_zone = self.floorplan.move_for(doorway, direction)
        changes = occupancy.apply_move(self.state, src_zone, dest_zone)
        if not changes:
            return  # intra-zone move: nothing countable happened
        if self.db is not None:
            self.db.save_occupancy(self.state)
        await self.hub.broadcast({
            "type": "occupancy_delta",
            # Absolute values, not diffs: idempotent, reconnect-safe.
            "changes": changes,
            "house_total": self.state.house_total,
            "clamp_count": self.state.clamp_count,
            "cause": {
                "kind": "crossing",
                "node_id": node_id,
                "seq": seq,
                "doorway_id": doorway.id,
                "direction": direction,
                "event_ts_ms": event_ts_ms,
            },
        })

    # ----- status / heartbeats -------------------------------------------

    async def handle_status(self, payload: bytes | str, arrival_ms: int | None = None) -> None:
        arrival_ms = arrival_ms if arrival_ms is not None else now_ms()
        try:
            raw = json.loads(payload)
            node_id = str(raw["node_id"])
            online = bool(raw["online"])
        except (ValueError, KeyError, TypeError) as exc:
            log.warning("dropping malformed status %r: %s", payload, exc)
            return

        prev = self.nodes.get(node_id)
        flipped = prev is None or prev.get("online") != online
        status = {
            "node_id": node_id,
            "online": online,
            "fw": raw.get("fw"),
            "uptime_s": raw.get("uptime_s"),
            "heap_free": raw.get("heap_free"),
            "pending": raw.get("pending"),
            "rssi": raw.get("rssi"),
            "time_synced": raw.get("time_synced"),
            "sync_age_s": raw.get("sync_age_s"),
            "last_seen_ms": arrival_ms,
        }
        self.nodes[node_id] = status

        if flipped and self.db is not None:
            self.db.insert_status_transition(node_id, online, arrival_ms, raw)
        # Broadcast every heartbeat, not just flips: the UI derives
        # last-seen freshness from it.
        await self.hub.broadcast({"type": "node_status", **status})

    # ----- snapshot --------------------------------------------------------

    def snapshot(self) -> dict:
        return {
            "type": "snapshot",
            "occupancy": dict(self.state.counts),
            "house_total": self.state.house_total,
            "clamp_count": self.state.clamp_count,
            "nodes": {nid: dict(s) for nid, s in self.nodes.items()},
            "recent_events": list(self.recent_events),
        }
