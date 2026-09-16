"""Sample-accurate scheduling against the dsp clock, pumped deterministically by the manual sink."""

from __future__ import annotations

import numpy as np

import bw_audio as bwa
from conftest import BLK, SR, tone, write_wav


def _block_energy(out):
    return float(np.sum(out.astype(np.float64) ** 2))


def test_play_at_lands_in_the_scheduled_block(manual_engine, tmp_path):
    """The manual sink's clock is a pure sample counter, so this is exact rather than best-effort."""
    e = manual_engine
    snd = e.load_sound(write_wav(tmp_path / "steady.wav", tone(SR // 4, amp=0.5)))

    src = e.create_source()
    src.set_pos(0.0, 1.5, 2.0)
    e.commit()

    # Advance the clock a couple of blocks so "now" is a real reading rather than the 0 before the
    # first block, then schedule three blocks out.
    for _ in range(2):
        e.render_block()
    now = e.dsp_time_frames
    lead_blocks = 3
    start = now + lead_blocks * BLK
    src.play_at(snd, start)

    energies = []
    for _ in range(lead_blocks + 3):
        energies.append(_block_energy(e.render_block()))

    # The block whose first sample IS `start` is the first that may sound. Everything before it is
    # silence, and the schedule is what puts it there - not luck.
    first_sounding = next(i for i, v in enumerate(energies) if v > 0.0)
    rendered_start = now + BLK  # the next render_block's first sample
    expected = (start - rendered_start) // BLK
    assert first_sounding == expected, (
        "scheduled start landed in block {0}, expected {1}: {2}".format(first_sounding, expected, energies)
    )
    assert all(v == 0.0 for v in energies[:first_sounding])
    snd.unload()


def test_stop_at_silences_within_a_block(manual_engine, tmp_path):
    e = manual_engine
    snd = e.load_sound(write_wav(tmp_path / "long.wav", tone(SR, amp=0.5)))
    src = e.create_source()
    src.set_pos(0.0, 1.5, 2.0)
    e.commit()
    src.play(snd)

    for _ in range(3):
        e.render_block()
    assert _block_energy(e.render_block()) > 0.0

    src.stop_at(e.dsp_time_frames + BLK)
    energies = [_block_energy(e.render_block()) for _ in range(6)]
    assert energies[-1] == 0.0, "a scheduled stop must reach silence within about a block"
    snd.unload()


def test_clock_advances_one_block_per_render(manual_engine):
    e = manual_engine
    e.render_block()
    first = e.dsp_time_frames
    e.render_block()
    assert e.dsp_time_frames - first == BLK


def test_manual_sink_clock_pair_is_wall_free(manual_engine):
    """The manual sink stamps a nominal, wall-free time: exact for arithmetic, not real wall time."""
    e = manual_engine
    for _ in range(4):
        e.render_block()
    pair = e.clock
    assert pair is not None
    dsp, host_ns = pair
    assert dsp == e.dsp_time_frames
    assert host_ns > 0

    model = e.clock_model
    if model is not None:
        # Synthesized from the sample position, so it fits exactly zero drift. True of the fiction,
        # not of any hardware.
        assert abs(model.ppm) < 1e-6


def test_loop_events_report_every_wrap(manual_engine, tmp_path):
    e = manual_engine
    short = BLK // 4  # several wraps inside one audio block
    snd = e.load_sound(write_wav(tmp_path / "short.wav", tone(short, freq=1000.0, amp=0.4)))
    src = e.create_source()
    src.set_pos(0.0, 1.5, 2.0)
    e.commit()
    src.play(snd, loop=True)

    wraps = 0
    for _ in range(4):
        e.render_block()
        e.commit()
        handles, dropped = e.poll_looped()
        assert dropped == 0
        wraps += sum(1 for h in handles if h == src.handle)
    assert wraps >= 3, "one entry per wrap, not per block"
    src.stop()
    snd.unload()


def test_output_latency_is_zero_without_a_physical_output(manual_engine):
    assert manual_engine.output_latency_frames == 0
    assert manual_engine.output_latency_seconds == 0.0
