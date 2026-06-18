"""Dedup window tests: QoS1 republish suppressed, node reboot not."""

from app.pipeline import DedupWindow


def test_first_sight_is_not_duplicate():
    win = DedupWindow()
    assert win.seen(1, 1000) is False


def test_republish_same_seq_same_t_us_is_duplicate():
    win = DedupWindow()
    win.seen(1, 1000)
    assert win.seen(1, 1000) is True


def test_reboot_same_seq_different_t_us_is_new():
    # Node reboot restarts seq near 0 but t_us is boot-relative, so the
    # (seq, t_us) pair differs -> must NOT be treated as a duplicate.
    win = DedupWindow()
    win.seen(1, 999_000_000)
    assert win.seen(1, 12_000_000) is False


def test_window_eviction():
    win = DedupWindow(size=4)
    for seq in range(5):  # seq 0 falls off the ring
        win.seen(seq, seq * 10)
    assert win.seen(0, 0) is False  # evicted -> counts as new again
    assert win.seen(4, 40) is True  # still in window
