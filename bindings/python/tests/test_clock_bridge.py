"""The host-clock read and the ClockBridge built on it.

The point of `bwa_host_time_ns` is that the offset between your clock and the engine's stops being
an estimate: the sandwich bounds its own error, so these tests can assert against that bound rather
than against a tolerance someone picked.
"""

from __future__ import annotations

import time

import bw_audio as bwa
from conftest import BLK, SR


def test_host_time_ns_needs_no_engine_and_advances():
    """It is a property of the process, so it answers before any Engine exists."""
    a = bwa.host_time_ns()
    b = bwa.host_time_ns()
    assert a > 0
    assert b >= a
    # Over a real sleep it must move by about that much, which rules out a stuck or wrong-scale clock.
    t0 = bwa.host_time_ns()
    time.sleep(0.05)
    elapsed_ms = (bwa.host_time_ns() - t0) * 1e-6
    assert 40.0 < elapsed_ms < 500.0, elapsed_ms


def test_offset_is_stable_across_refreshes_within_the_bound(null_engine):
    """|off_i - off_j| <= err_i + err_j is arithmetic, not a tolerance: each sandwich brackets the
    true offset. A failure here means the two clocks are not the same clock, or the read is wrong."""
    c = null_engine.bridge()
    samples = []
    for _ in range(8):
        err = c.refresh()
        samples.append((c.offset_ns, err))
        time.sleep(0.002)
    for i in range(len(samples)):
        for j in range(i + 1, len(samples)):
            oi, ei = samples[i]
            oj, ej = samples[j]
            # 1 us of slack for the ns rounding in the platform conversion.
            assert abs(oi - oj) <= ei + ej + 1000.0, (samples[i], samples[j])


def test_the_error_bound_is_small_on_this_machine(null_engine):
    """Reported, not just asserted: the bound is the number that says how good the anchor is."""
    c = null_engine.bridge()
    print("two-reads error bound: {0:.2f} us".format(c.error_ns * 1e-3))
    assert c.error_ns < 100e3, c.error_ns


def test_perf_counter_is_the_engine_clock(null_engine):
    """On every platform the engine's clock IS time.perf_counter's, so the offset is near zero.

    Not a contract the bridge depends on (it measures whatever offset there is), but it is the
    reason perf_counter is the default, and a large value here would say the platform changed.
    """
    c = null_engine.bridge()
    assert abs(c.offset_ns) < 1e9, c.offset_ns


def test_dsp_at_and_time_at_round_trip(manual_engine):
    e = manual_engine
    for _ in range(4):
        e.render_block()
    c = e.bridge()
    for lead in (0.0, 0.01, 0.5, 5.0):
        t = time.perf_counter() + lead
        assert abs(c.time_at(c.dsp_at(t)) - t) < 1e-3, lead


def test_dsp_at_of_the_stamps_own_host_time_is_the_stamps_sample(manual_engine):
    """The manual sink's stamps are synthesized from the sample position, so this is exact.

    Feed the bridge the caller-clock time that maps to the pair's own host time and it must hand
    back the pair's own sample, with no drift term and no rounding past a sample.
    """
    e = manual_engine
    for _ in range(4):
        e.render_block()
    pair = e.clock
    assert pair is not None
    cs, _ct = pair
    c = e.bridge()
    assert abs(c.dsp_at(c.time_at(cs)) - cs) <= 1


def test_a_caller_clock_with_a_huge_epoch_offset_works(manual_engine):
    """The bridge measures the offset, so an arbitrary epoch costs nothing."""
    e = manual_engine
    for _ in range(4):
        e.render_block()
    shift = 1.0e6                       # a clock whose zero is 11 days from perf_counter's
    plain = e.bridge()
    shifted = e.bridge(clock=lambda: time.perf_counter() + shift)
    t = time.perf_counter() + 0.1
    assert abs(shifted.offset_ns - (plain.offset_ns - shift * 1e9)) < 1e6
    # Same instant described in two clocks must land on the same sample, to within a block.
    assert abs(shifted.dsp_at(t + shift) - plain.dsp_at(t)) < BLK


def test_dsp_at_answers_before_the_first_stamped_block(null_engine):
    """No pair yet is a fallback, not a failure: pair the block counter with the caller's clock."""
    c = null_engine.bridge()
    assert null_engine.clock is None
    assert c.dsp_at(time.perf_counter()) >= 0
    assert c.dsp_at(time.perf_counter() - 1e6) == 0      # a past time clamps rather than going negative


def test_dsp_at_now_tracks_the_engine_clock():
    """On a host-paced sink the stamps are real host times, so "now" on the caller's clock must be
    the engine's own now. This is the assertion the whole call exists for."""
    e = bwa.Engine(profile=bwa.Profile.CAVE, sample_rate=SR, block_size=BLK, sink=bwa.SinkType.NULL)
    try:
        e.start()
        deadline = time.perf_counter() + 2.0
        while e.clock is None and time.perf_counter() < deadline:
            time.sleep(0.01)
        assert e.clock is not None, "the null sink never stamped a block"
        c = e.bridge()
        err = c.dsp_at(time.perf_counter()) - e.dsp_time_frames
        print("dsp_at(now) - dsp_time_frames: {0} frames".format(err))
        # dsp_time_frames is the LAST block's first sample, so "now" is legitimately up to a block
        # and a bit ahead of it. Four blocks covers a preempted interpreter; an epoch error would
        # be millions.
        assert -BLK < err < 4 * BLK, err
    finally:
        e.close()


def test_play_at_takes_off_the_output_latency(manual_engine, tmp_path):
    from conftest import tone, write_wav

    e = manual_engine
    snd = e.load_sound(write_wav(tmp_path / "click.wav", tone(SR // 10)))
    src = e.create_source()
    src.set_pos(0.0, 1.5, 2.0)
    e.commit()
    for _ in range(4):
        e.render_block()

    c = e.bridge()
    t = c.time_at(e.dsp_time_frames + 10 * BLK)
    want = c.dsp_at(t)
    start = c.play_at(src, snd, t)
    assert start == max(0, want - e.output_latency_frames)
    assert start > e.dsp_time_frames                      # still in the future, so it is schedulable


def test_drift_ppm_is_none_or_a_number(manual_engine):
    """The fit needs about a second of stamps. Before that the property says so rather than lying."""
    ppm = manual_engine.bridge().drift_ppm
    assert ppm is None or isinstance(ppm, float)


def test_refresh_returns_the_bound_it_set(null_engine):
    c = null_engine.bridge()
    err = c.refresh(rounds=3)
    assert err == c.error_ns
    assert err > 0.0


def test_bridge_repr_is_ascii(null_engine):
    r = repr(null_engine.bridge())
    assert r.isascii()
    assert "ClockBridge" in r
