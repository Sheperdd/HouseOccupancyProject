"""Pure occupancy model tests — no IO, no asyncio. Counts live on zones."""

import pytest

from app.occupancy import OccupancyState, apply_move, reset


def fresh_state() -> OccupancyState:
    return OccupancyState({"main": 0, "bedroom1": 0})


def test_move_transfers_between_zones():
    state = fresh_state()
    state.counts["main"] = 1
    changes = apply_move(state, "main", "bedroom1")
    assert state.counts == {"main": 0, "bedroom1": 1}
    assert changes == {"bedroom1": 1, "main": 0}


def test_outside_leg_is_noop():
    state = fresh_state()
    changes = apply_move(state, "outside", "main")  # person enters the house
    assert state.counts == {"main": 1, "bedroom1": 0}
    assert "outside" not in changes
    apply_move(state, "main", "outside")  # leaves
    assert state.counts == {"main": 0, "bedroom1": 0}
    assert "outside" not in state.counts


def test_same_zone_move_is_noop():
    state = fresh_state()
    state.counts["main"] = 1
    assert apply_move(state, "main", "main") == {}
    assert state.counts == {"main": 1, "bedroom1": 0}
    assert state.clamp_count == 0


def test_decrement_clamps_at_zero_and_counts_it():
    state = fresh_state()
    apply_move(state, "main", "bedroom1")  # main was 0 -> clamp
    assert state.counts == {"main": 0, "bedroom1": 1}
    assert state.clamp_count == 1
    apply_move(state, "main", "bedroom1")
    assert state.clamp_count == 2


def test_house_total_is_sum():
    state = fresh_state()
    apply_move(state, "outside", "main")
    apply_move(state, "outside", "main")
    apply_move(state, "main", "bedroom1")
    assert state.house_total == 2


def test_reset_zeroes_everything():
    state = fresh_state()
    apply_move(state, "outside", "main")
    state.clamp_count = 3
    new = reset(state)
    assert new == {"main": 0, "bedroom1": 0}
    assert state.house_total == 0
    # clamp_count survives reset on purpose: it's lifetime drift telemetry,
    # not occupancy state.
    assert state.clamp_count == 3


def test_reset_with_explicit_counts():
    state = fresh_state()
    new = reset(state, {"bedroom1": 2})
    assert new == {"main": 0, "bedroom1": 2}


def test_reset_rejects_unknown_zone_and_negative():
    with pytest.raises(KeyError):
        reset(fresh_state(), {"garage": 1})
    with pytest.raises(ValueError):
        reset(fresh_state(), {"bedroom1": -1})
