"""Floorplan loader validation + the committed real layout parses correctly."""

from pathlib import Path

import pytest

from app.floorplan import Floor, Floorplan, FloorplanError, Doorway, Room, load_floorplan

BACKEND_DIR = Path(__file__).resolve().parent.parent


def room(rid: str, zone: str | None = None) -> Room:
    return Room(id=rid, name=rid, zone=zone or rid, x=0, y=0, w=1, h=1)


def test_committed_real_layout():
    fp = load_floorplan(BACKEND_DIR / "floorplan.json")
    # Two sensors -> three zones: main, bedroom1 (+ implicit outside)
    assert fp.zone_ids() == ["main", "bedroom1"]
    # All zone boundaries are sensored -> every count is exact
    assert fp.zones_with_unsensored_exits() == set()
    # node 01: in -> bedroom1, from the main-floor zone
    dw1 = fp.doorway_for_node("doorway-node-01")
    assert fp.move_for(dw1, "in") == ("main", "bedroom1")
    assert fp.move_for(dw1, "out") == ("bedroom1", "main")
    # node 02: in -> the house, from outside
    dw2 = fp.doorway_for_node("doorway-node-02")
    assert fp.move_for(dw2, "in") == ("outside", "main")
    assert fp.move_for(dw2, "out") == ("main", "outside")


def test_sensored_doorway_within_one_zone_rejected():
    # A sensor between two rooms of the same zone could never change a
    # count — config error, catch it at load.
    floors = [Floor("f1", "F1", (room("a", "z"), room("b", "z")))]
    dw = Doorway("d", ("a", "b"), "node-x", "a", "f1", 0, 0)
    with pytest.raises(FloorplanError):
        Floorplan(floors, [dw], raw={})


def test_unsensored_zone_boundary_marks_both_zones_leaky():
    floors = [Floor("f1", "F1", (room("a"), room("b")))]
    dw = Doorway("d", ("a", "b"), None, "a", "f1", 0, 0)
    fp = Floorplan(floors, [dw], raw={})
    assert fp.zones_with_unsensored_exits() == {"a", "b"}


def test_doorway_unknown_room_rejected():
    floors = [Floor("f1", "F1", (room("a"),))]
    dw = Doorway("d", ("a", "b"), None, "a", "f1", 0, 0)
    with pytest.raises(FloorplanError):
        Floorplan(floors, [dw], raw={})


def test_in_room_must_belong_to_doorway():
    floors = [Floor("f1", "F1", (room("a"), room("b"), room("c")))]
    dw = Doorway("d", ("a", "b"), None, "c", "f1", 0, 0)
    with pytest.raises(FloorplanError):
        Floorplan(floors, [dw], raw={})


def test_node_assigned_twice_rejected():
    floors = [Floor("f1", "F1", (room("a"), room("b")))]
    d1 = Doorway("d1", ("a", "b"), "node-x", "a", "f1", 0, 0)
    d2 = Doorway("d2", ("b", "a"), "node-x", "b", "f1", 0, 0)
    with pytest.raises(FloorplanError):
        Floorplan(floors, [d1, d2], raw={})


def test_outside_not_a_room_or_zone():
    with pytest.raises(FloorplanError):
        Floorplan([Floor("f1", "F1", (room("outside"),))], [], raw={})
    with pytest.raises(FloorplanError):
        Floorplan([Floor("f1", "F1", (room("a", "outside"),))], [], raw={})
