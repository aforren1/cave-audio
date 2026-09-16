"""The threading contract: one control thread, and the GIL released around the blocking calls."""

from __future__ import annotations

import threading
import time

import numpy as np
import pytest

import bw_audio as bwa
from conftest import SR, write_wav


def _call_on_a_new_thread(fn):
    """Run fn on a fresh thread and hand back whatever it raised, or None."""
    box = {}

    def run():
        try:
            fn()
        except BaseException as exc:  # noqa: BLE001 - the exception IS the result
            box["exc"] = exc

    t = threading.Thread(target=run)
    t.start()
    t.join(10.0)
    assert not t.is_alive()
    return box.get("exc")


def test_a_second_thread_is_refused(null_engine):
    exc = _call_on_a_new_thread(lambda: null_engine.commit())
    assert isinstance(exc, bwa.BwaError)
    assert "ONE control thread" in str(exc)


def test_the_guard_covers_handles_too(manual_engine):
    src = manual_engine.create_push_source()
    exc = _call_on_a_new_thread(lambda: src.set_pos(0.0, 1.0, 0.0))
    assert isinstance(exc, bwa.BwaError)


def test_the_guard_can_be_opted_out_of():
    """For a caller who already funnels their own calls through one thread."""
    e = bwa.Engine(sink=bwa.SinkType.NULL, thread_guard=False)
    try:
        assert _call_on_a_new_thread(lambda: e.commit()) is None
    finally:
        e.close()


def test_the_raw_layer_has_no_guard(null_engine):
    """Documented: the guard is the Pythonic layer's. The raw layer is the ABI and nothing else."""
    assert _call_on_a_new_thread(lambda: bwa._bwa.commit(null_engine.raw)) is None


def test_a_blocking_call_releases_the_gil(manual_engine, tmp_path):
    """A second Python thread must make progress WHILE a load runs.

    If the GIL were held across the C call, no other Python thread could execute a single
    bytecode, so the counter would be exactly unchanged. Any advance is therefore proof rather
    than a coin flip, and the margin below is only there to reject a fluke scheduling artifact.
    """
    # A long asset, so the decode is measurably longer than a scheduling quantum.
    path = write_wav(tmp_path / "long.wav", np.zeros(SR * 120, dtype=np.float32))

    counter = {"n": 0}
    stop = threading.Event()

    def spin():
        while not stop.is_set():
            counter["n"] += 1

    t = threading.Thread(target=spin, daemon=True)
    t.start()
    time.sleep(0.05)  # let it reach a steady rate

    c0, t0 = counter["n"], time.perf_counter()
    snd = manual_engine.load_sound(path)
    c1, t1 = counter["n"], time.perf_counter()

    stop.set()
    t.join(5.0)
    snd.unload()

    elapsed = t1 - t0
    advanced = c1 - c0
    if elapsed < 0.010:
        pytest.skip(
            "the decode took {0:.1f} ms, too little for this machine to distinguish a released "
            "GIL from a lucky handoff; the assertion would imply coverage it does not have"
            .format(elapsed * 1e3)
        )
    assert advanced > 100, (
        "the counter thread advanced {0} times in {1:.1f} ms of load_sound - a held GIL would "
        "leave it at exactly 0".format(advanced, elapsed * 1e3)
    )


def test_render_block_releases_the_gil_too(manual_engine):
    """Same argument, applied to the offline render loop an experiment actually runs."""
    counter = {"n": 0}
    stop = threading.Event()

    def spin():
        while not stop.is_set():
            counter["n"] += 1

    t = threading.Thread(target=spin, daemon=True)
    t.start()
    time.sleep(0.05)

    c0, t0 = counter["n"], time.perf_counter()
    for _ in range(2000):
        manual_engine.render_block()
    c1, t1 = counter["n"], time.perf_counter()

    stop.set()
    t.join(5.0)

    elapsed = t1 - t0
    if elapsed < 0.010:
        pytest.skip("2000 blocks rendered in {0:.1f} ms; too fast to judge".format(elapsed * 1e3))
    assert c1 - c0 > 100
