"""Assets and push sources: a wav the test writes itself, and a numpy-fed procedural voice."""

from __future__ import annotations

import numpy as np
import pytest

import bw_audio as bwa
from conftest import SR, tone, write_wav


def test_load_a_wav_the_test_wrote(manual_engine, tmp_path):
    e = manual_engine
    path = write_wav(tmp_path / "beep.wav", tone(SR // 2))  # 0.5 s

    snd = e.load_sound(path)
    assert int(snd) != 0
    assert snd.channels == 1
    assert snd.frames == SR // 2
    assert snd.seconds == pytest.approx(0.5, abs=1e-6)

    src = e.create_source()
    src.set_pos(0.0, 1.5, 2.0)
    e.commit()
    src.play(snd)

    peak = 0.0
    for _ in range(8):
        out = e.render_block()
        peak = max(peak, float(np.abs(out).max()))
    assert peak > 0.0
    assert src.is_playing
    assert src.playhead_frames > 0

    snd.unload()


def test_missing_file_raises_with_the_engine_reason(manual_engine):
    with pytest.raises(bwa.BwaError) as exc:
        manual_engine.load_sound("does/not/exist.wav")
    assert "does/not/exist.wav" in str(exc.value)


def test_shared_acquire_returns_the_same_handle(manual_engine, tmp_path):
    e = manual_engine
    path = write_wav(tmp_path / "shared.wav", tone(1024))

    a = e.acquire(path)
    b = e.acquire(path)
    assert int(a) == int(b), "the same (path, flags) key must return the same handle"
    assert e.find(path) is not None
    assert int(e.find(path)) == int(a)

    a.release()
    b.release()
    assert e.find(path) is None


def test_find_never_loads(manual_engine, tmp_path):
    path = write_wav(tmp_path / "unseen.wav", tone(512))
    assert manual_engine.find(path) is None, "find is a pure lookup; a miss must not decode"


def test_push_source_from_a_numpy_array(manual_engine):
    e = manual_engine
    s = e.create_push_source()
    s.set_pos(1.0, 1.5, 0.0)
    e.commit()

    assert s.space > 0
    block = np.full(e.block_size, 0.25, dtype=np.float32)
    assert s.push(block) == e.block_size

    out = e.render_block()
    assert float(np.abs(out).max()) > 0.0


def test_push_accepts_a_float64_array(manual_engine):
    """float32 is the ABI's dtype; anything else is converted, at the cost of a temporary copy.

    The Pythonic layer converts explicitly. The raw layer gets nanobind's own implicit conversion
    on the second overload pass, which is why the raw call below also succeeds - but a 2-D array
    is refused either way, because a push is a mono frame stream and a silent reshape would hide
    a caller bug.
    """
    e = manual_engine
    s = e.create_push_source()
    e.commit()
    n = s.push(np.linspace(-0.2, 0.2, e.block_size, dtype=np.float64))
    assert n == e.block_size

    assert bwa._bwa.source_push(e.raw, s.handle, np.zeros(16, dtype=np.float64)) == 16

    with pytest.raises(Exception):
        bwa._bwa.source_push(e.raw, s.handle, np.zeros((4, 4), dtype=np.float32))


def test_push_reports_a_full_ring(manual_engine):
    e = manual_engine
    s = e.create_push_source()
    e.commit()
    space = s.space
    big = np.zeros(space + 4096, dtype=np.float32)
    assert s.push(big) == space, "push must report the count accepted, not the count offered"
    assert s.space == 0


def test_push_end_ends_the_voice(manual_engine):
    e = manual_engine
    s = e.create_push_source()
    e.commit()
    s.push(np.full(512, 0.1, dtype=np.float32))
    s.push_end()
    for _ in range(16):
        e.render_block()
        e.commit()
    ended, dropped = e.poll_ended()
    assert s.handle in ended
    assert dropped == 0


def test_absurd_position_is_a_no_op(manual_engine):
    """Finite but absurd is its own defect class: the bound is +/-1e6 m and the call no-ops."""
    e = manual_engine
    s = e.create_push_source()
    s.set_pos(1.0, 1.5, 0.0)
    e.commit()
    s.push(np.full(e.block_size, 0.25, dtype=np.float32))
    good = np.array(e.render_block(), copy=True)

    s.set_pos(3e38, 1.5, 0.0)  # refused; the previous position stands
    e.commit()
    s.push(np.full(e.block_size, 0.25, dtype=np.float32))
    after = e.render_block()
    assert np.all(np.isfinite(after)), "an absurd position must never reach the panner"
    assert float(np.abs(after).max()) > 0.0
    assert good.shape == after.shape
