"""Floor plan graph loaded from floorplan.json.

The layout is data, not code: rooms (schematic rects per floor), doorways
connecting room pairs, and which doorway each sensor node covers.

ZONES: occupancy counts live on zones, not rooms. Sensors can only
distinguish regions separated by sensored doorways — with two sensors this
house has exactly three: outside, the main floor, bedroom 1. Rooms declare
`"zone"` to share a counting region (default: a room is its own zone).
Doorways between rooms of the SAME zone are invisible to counting and exist
only for the drawing; a SENSORED doorway whose rooms share a zone is a
config error (the sensor could never change any count).
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

# Reserved pseudo-room AND pseudo-zone: people who go "outside" leave the
# model entirely. Never counted, never rendered.
OUTSIDE = "outside"


@dataclass(frozen=True)
class Room:
    id: str
    name: str
    zone: str
    x: float
    y: float
    w: float
    h: float


@dataclass(frozen=True)
class Floor:
    id: str
    name: str
    rooms: tuple[Room, ...]


@dataclass(frozen=True)
class Doorway:
    id: str
    rooms: tuple[str, str]
    node_id: str | None  # None = unsensored (people move through unseen)
    in_room: str         # the room a node's "in" direction points into
    floor: str
    x: float
    y: float

    def other_room(self, room: str) -> str:
        return self.rooms[1] if room == self.rooms[0] else self.rooms[0]


class FloorplanError(ValueError):
    pass


class Floorplan:
    def __init__(self, floors: list[Floor], doorways: list[Doorway], raw: dict):
        self.floors = floors
        self.doorways = doorways
        self.raw = raw  # served verbatim by GET /api/floorplan
        self._rooms: dict[str, Room] = {}
        self._zone_of: dict[str, str] = {OUTSIDE: OUTSIDE}
        for floor in floors:
            for room in floor.rooms:
                if room.id in self._rooms:
                    raise FloorplanError(f"duplicate room id {room.id!r}")
                if room.id == OUTSIDE or room.zone == OUTSIDE:
                    raise FloorplanError(f"{OUTSIDE!r} is reserved, not a real room/zone")
                self._rooms[room.id] = room
                self._zone_of[room.id] = room.zone

        self._by_node: dict[str, Doorway] = {}
        floor_ids = {f.id for f in floors}
        for dw in doorways:
            for r in dw.rooms:
                if r != OUTSIDE and r not in self._rooms:
                    raise FloorplanError(f"doorway {dw.id!r} references unknown room {r!r}")
            if dw.in_room not in dw.rooms:
                raise FloorplanError(f"doorway {dw.id!r}: in_room must be one of its two rooms")
            if dw.floor not in floor_ids:
                raise FloorplanError(f"doorway {dw.id!r} references unknown floor {dw.floor!r}")
            if dw.node_id is not None:
                if dw.node_id in self._by_node:
                    raise FloorplanError(f"node {dw.node_id!r} assigned to two doorways")
                if self.zone_of(dw.rooms[0]) == self.zone_of(dw.rooms[1]):
                    raise FloorplanError(
                        f"doorway {dw.id!r} is sensored but both rooms are in zone "
                        f"{self.zone_of(dw.rooms[0])!r} — the sensor could never "
                        "change a count; split the zone or remove the node"
                    )
                self._by_node[dw.node_id] = dw

    def zone_of(self, room_id: str) -> str:
        return self._zone_of[room_id]

    def zone_ids(self) -> list[str]:
        seen: dict[str, None] = {}  # ordered de-dup
        for room in self._rooms.values():
            seen.setdefault(room.zone)
        return list(seen)

    def doorway_for_node(self, node_id: str) -> Doorway | None:
        return self._by_node.get(node_id)

    def move_for(self, doorway: Doorway, direction: str) -> tuple[str, str]:
        """Resolve a crossing to a (src_zone, dest_zone) move."""
        if direction == "in":
            dest_room = doorway.in_room
        elif direction == "out":
            dest_room = doorway.other_room(doorway.in_room)
        else:
            raise ValueError(f"direction must be 'in' or 'out', got {direction!r}")
        src_room = doorway.other_room(dest_room)
        return self.zone_of(src_room), self.zone_of(dest_room)

    def zones_with_unsensored_exits(self) -> set[str]:
        """Zones whose count is approximate: an UNSENSORED doorway crosses
        the zone boundary, so people can enter/leave unseen. (Unsensored
        doorways WITHIN a zone don't count — that's the point of zones.)"""
        leaky: set[str] = set()
        for dw in self.doorways:
            if dw.node_id is not None:
                continue
            za, zb = self.zone_of(dw.rooms[0]), self.zone_of(dw.rooms[1])
            if za != zb:
                leaky.update(z for z in (za, zb) if z != OUTSIDE)
        return leaky


def load_floorplan(path: str | Path) -> Floorplan:
    raw = json.loads(Path(path).read_text(encoding="utf-8"))
    floors = [
        Floor(
            id=f["id"],
            name=f.get("name", f["id"]),
            rooms=tuple(
                Room(id=r["id"], name=r.get("name", r["id"]),
                     zone=r.get("zone", r["id"]),
                     x=r["x"], y=r["y"], w=r["w"], h=r["h"])
                for r in f["rooms"]
            ),
        )
        for f in raw["floors"]
    ]
    doorways = [
        Doorway(
            id=d["id"],
            rooms=(d["rooms"][0], d["rooms"][1]),
            node_id=d.get("node_id"),
            in_room=d["in_room"],
            floor=d["floor"],
            x=d.get("x", 0.0),
            y=d.get("y", 0.0),
        )
        for d in raw["doorways"]
    ]
    return Floorplan(floors, doorways, raw)
