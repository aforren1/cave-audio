"""The manual sink: the shape of a rendered block, and the GOLDEN.

The golden is test/golden_test.c's own scenario and its own committed constants, driven from
Python. That is the point: it proves the binding reaches the same DSP the C suite pins, so a
binding bug that quietly changed a gain or a position cannot pass. Reproducing it needed no
C-only step - a push source, a position, a pose, a commit and a render loop are all bound.
"""

from __future__ import annotations

import numpy as np
import pytest

import bw_audio as bwa
from conftest import BLK, SR, golden_tone_block

# test/golden_test.c's committed references and its tolerance. Regenerate BOTH together, from that
# test's printout, after an intended DSP change.
G_TOTAL = 168.172397
G_NEGX = 167.502218
G_TOL = 2e-3
NBLK = 48


def _render_golden_scenario():
    """test/golden_test.c's render_scenario, call for call."""
    e = bwa.Engine(profile=bwa.Profile.CAVE, sample_rate=SR, block_size=BLK, sink=bwa.SinkType.MANUAL)
    try:
        nch = e.channel_count
        spk = np.array(e.speakers, copy=True)

        s = e.create_push_source()
        s.set_pos(-1.5, 1.5, 0.0)  # room -x is the listener's RIGHT
        e.listener.set_pose(0.0, 1.5, 0.0, 0.0, 0.0, 0.0, 1.0)
        e.commit()
        e.start()

        energy = np.zeros(nch, dtype=np.float64)
        head = []
        for b in range(NBLK):
            s.push(golden_tone_block(b))
            out = e.render_block()
            assert out is not None and out.shape == (nch, BLK)
            energy += np.sum(out.astype(np.float64) ** 2, axis=1)
            if len(head) < BLK:
                head.append(np.array(out[0], copy=True))

        # The manual sink cannot observe a dropout, and must say so rather than report a clean
        # bill. This is the offline blind spot the counters exist to be honest about.
        assert e.health is None

        total = float(energy.sum())
        neg_x = float(energy[spk[:, 0] < -0.5].sum())
        pos_x = float(energy[spk[:, 0] > 0.5].sum())
        return total, neg_x, pos_x, np.concatenate(head)
    finally:
        e.close()


def test_golden_matches_the_c_suite():
    total, neg_x, pos_x, _ = _render_golden_scenario()
    assert abs(total - G_TOTAL) <= G_TOL * G_TOTAL, (
        "golden total {0:.6f} against {1:.6f}. If the DSP changed on purpose, regenerate this AND "
        "test/golden_test.c's constants from that test's printout.".format(total, G_TOTAL)
    )
    assert abs(neg_x - G_NEGX) <= G_TOL * G_NEGX
    assert neg_x > pos_x * 1.1, "a room -x source must be right-biased"


def test_render_is_reproducible():
    """Two independent renders are bit-identical. That is what makes a committed golden mean anything."""
    a = _render_golden_scenario()
    b = _render_golden_scenario()
    assert a[0] == b[0] and a[1] == b[1] and a[2] == b[2]
    assert np.array_equal(a[3], b[3])


def test_block_shape_dtype_and_read_only(manual_engine):
    e = manual_engine
    out = e.render_block()
    assert out.shape == (e.channel_count, e.block_size)
    assert out.dtype == np.float32
    assert not out.flags.writeable
    with pytest.raises(ValueError):
        out[0, 0] = 1.0


def test_block_is_a_view_that_the_next_render_overwrites(manual_engine):
    """The array aliases the engine's own buffer. Copy it if you keep it - the docstring says so,
    and this is what makes that claim testable rather than a promise."""
    e = manual_engine
    s = e.create_push_source()
    s.set_pos(0.0, 1.5, 1.0)
    e.commit()

    s.push(np.full(e.block_size, 0.5, dtype=np.float32))
    first = e.render_block()
    kept = first.copy()
    assert np.abs(kept).max() > 0.0

    s.push(np.zeros(e.block_size, dtype=np.float32))
    e.render_block()
    assert not np.array_equal(first, kept), "render_block should hand back a view, not a copy"

    # And copy=True survives the next render, which is the whole reason it exists.
    s.push(np.full(e.block_size, 0.5, dtype=np.float32))
    owned = e.render_block(copy=True)
    snapshot = np.array(owned, copy=True)
    s.push(np.zeros(e.block_size, dtype=np.float32))
    e.render_block()
    assert np.array_equal(owned, snapshot)


def test_render_block_returns_none_off_the_manual_sink(null_engine):
    null_engine.start()
    assert null_engine.render_block() is None


def test_binaural_profile_renders_stereo():
    with bwa.Engine(profile=bwa.Profile.BINAURAL, sample_rate=SR, block_size=BLK,
                    sink=bwa.SinkType.MANUAL) as e:
        out = e.render_block()
        assert out.shape == (2, BLK)


def test_bus_levels_track_the_render(manual_engine):
    e = manual_engine
    s = e.create_push_source()
    s.set_pos(-1.5, 1.5, 0.0)
    e.commit()
    for b in range(4):
        s.push(golden_tone_block(b))
        e.render_block()
    peaks = e.bus_levels
    assert peaks.shape == (e.channel_count,)
    assert float(peaks.max()) > 0.0
    assert e.active_voices >= 1
