#!/usr/bin/env python
"""The smallest realistic bw_audio client: orbit a click around the listener's head, then quit.

This is the same demo `examples/minimal.c` runs, in the Pythonic layer. Every binding has a copy
and they all play the same stimulus (`stimulus.py`), so you can A/B a binding against the C
reference by ear.

The shape: an `Engine` context manager on the binaural profile, one push source fed the click,
a six-second horizontal lap at ear height two meters out, then the dropout counters.

Positions are commit-gated in the engine, and this layer commits them for you, so the loop below
is just a set and a push. Wrap several writes in `with e.frame():` when they must land as one
snapshot (README, "Commit model").

Run it:      uv run examples/minimal.py
Check it:    uv run examples/minimal.py --tests
"""

from __future__ import annotations

import argparse
import math
import sys
import time

import numpy as np

import bw_audio as bwa
import stimulus

SR = 48000
BLK = 256
RADIUS_M = 2.0            # how far out the click orbits
EAR_HEIGHT_M = 1.5        # the default grid's center, and the listener's ears
LAP_SECONDS = 6.0         # one revolution


def orbit_pos(t_seconds: float):
    """Where the click is at `t_seconds`. Horizontal, one lap in LAP_SECONDS.

    Starts in front (+z) and passes the LEFT ear first, because +x is left for an identity
    listener. Room space is right-handed, +y up, +z forward, meters, origin on the floor.
    """
    a = 2.0 * math.pi * t_seconds / LAP_SECONDS
    return RADIUS_M * math.sin(a), EAR_HEIGHT_M, RADIUS_M * math.cos(a)


def run(sink, seconds=LAP_SECONDS, device=None, verbose=True):
    """Play one lap. Returns the wall seconds it covered and the engine's dsp advance in frames."""
    with bwa.Engine(profile=bwa.Profile.BINAURAL, sample_rate=SR, block_size=BLK,
                    sink=sink, device=device) as e:
        e.start()
        if verbose:
            # A start that SUCCEEDS can still have degraded to the silent offline sink. The SINK
            # TYPE says so, not the backend string: the string names whichever backend opened,
            # and every real one makes sound.
            silent = " (no output device opened - silent run)" if e.sink_type == bwa.SinkType.NULL else ""
            print("backend: {0}{1}".format(e.backend, silent))
            print("output latency: {0} frames ({1:.1f} ms)".format(
                e.output_latency_frames, e.output_latency_seconds * 1e3))

        period = stimulus.click_period(e.sample_rate)
        src = e.create_push_source()
        src.set_gain(0.8)
        e.listener.set_pose(0.0, EAR_HEIGHT_M, 0.0)   # standing at the array center, facing +z

        dsp0 = e.dsp_time_frames
        cursor = 0
        t0 = time.perf_counter()
        t = 0.0
        while t < seconds:
            t = time.perf_counter() - t0
            src.set_pos(*orbit_pos(t))

            # Feed the ring from the click period, wrapping. Pace it with `space` rather than
            # with a fixed chunk: a push that does not fit is truncated, and the voice would
            # then skip part of a burst instead of underrunning cleanly.
            room = src.space
            while room > 0:
                take = min(room, period.size - cursor)
                took = src.push(period[cursor:cursor + take])
                if took == 0:
                    break
                cursor = (cursor + took) % period.size
                room -= took

            time.sleep(0.016)                         # ~60 Hz, like a game tick

        src.stop()
        dsp = e.dsp_time_frames - dsp0
        if verbose:
            h = e.health
            if h is None:
                print("dropout counters: NOT measurable on this sink")
            else:
                print("xruns {0} in {1} blocks, peak load {2:.2f}".format(
                    h.xruns, h.blocks, h.peak_load))
        return t, dsp


def render_lap(seconds=LAP_SECONDS, sample_rate=SR, block_size=BLK):
    """The same lap on the manual sink, so a test can look at what came out. (channels, frames)."""
    e = bwa.Engine(profile=bwa.Profile.BINAURAL, sample_rate=sample_rate, block_size=block_size,
                   sink=bwa.SinkType.MANUAL)
    try:
        period = stimulus.click_period(sample_rate)
        src = e.create_push_source()
        src.set_gain(0.8)
        e.listener.set_pose(0.0, EAR_HEIGHT_M, 0.0)
        e.start()

        out = []
        cursor = 0
        for b in range(int(round(seconds * sample_rate / block_size))):
            t0 = b * block_size
            src.set_pos(*orbit_pos(t0 / sample_rate))
            take = min(block_size, period.size - cursor)
            src.push(period[cursor:cursor + take])
            if take < block_size:
                src.push(period[:block_size - take])
            cursor = (cursor + block_size) % period.size
            out.append(np.array(e.render_block(), copy=True))   # the view dies at the next call
        return np.concatenate(out, axis=1)
    finally:
        e.close()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--device", default=None, help="exact device name or id; see live_onset.py --list-devices")
    ap.add_argument("--seconds", type=float, default=LAP_SECONDS)
    ap.add_argument("--tests", action="store_true",
                    help="self-check mode: force the offline sink, assert, exit nonzero on a miss")
    args = ap.parse_args(argv)

    if args.tests:
        return selftest()

    # AUTO picks by channel count and platform and falls back to the silent offline sink, so this
    # runs with or without headphones plugged in.
    print("orbiting the click around the listener's head at ear height ({0:.0f} s, one lap)...".format(
        args.seconds))
    run(bwa.SinkType.AUTO, seconds=args.seconds, device=args.device)
    print("finished")
    return 0


def selftest():
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        print("{0:<52} {1}{2}".format(name, "ok" if ok else "FAIL", ("  " + detail) if detail else ""))
        if not ok:
            failures += 1

    # ---- the stimulus. Every binding's copy must agree with these numbers. ----
    period = stimulus.click_period(SR)
    check("the click period is 250 ms", period.size == 12000, str(period.size))
    check("the burst peaks at -12 dBFS", abs(float(np.abs(period).max()) - stimulus.PEAK) < 1e-6,
          "{0:.6f}".format(float(np.abs(period).max())))
    check("the burst is 2 ms and the rest is silent",
          float(np.abs(period[96:]).max()) == 0.0 and float(np.abs(period[:96]).max()) > 0.0)
    # Not a tone: a 2 ms burst has energy across the band. Compare the top octave against the
    # bottom one, which a sine at any single frequency would fail by orders of magnitude.
    mag = np.abs(np.fft.rfft(period[:96].astype(np.float64)))
    low, high = mag[:24], mag[24:]
    check("the burst is broadband, not a tone", float(high.max()) > 0.2 * float(low.max()),
          "{0:.4f} against {1:.4f}".format(float(high.max()), float(low.max())))
    check("the stimulus is deterministic", np.array_equal(period, stimulus.click_period(SR)))

    # ---- the live path, on the offline sink: no device, but a real audio thread. ----
    wall, dsp = run(bwa.SinkType.NULL, seconds=0.5, verbose=False)
    check("the lap ran for about the time asked for", 0.5 <= wall < 1.5, "{0:.3f} s".format(wall))
    # The null sink is host-paced, so its dsp clock tracks the wall clock. Generous bounds: this
    # asserts the engine kept rendering, not the jitter of a sleeping interpreter.
    check("the engine's dsp clock advanced with it", 0.5 * wall * SR < dsp < 2.0 * wall * SR,
          "{0} frames over {1:.3f} s".format(dsp, wall))

    # ---- what came out. The demo's whole claim is that the click MOVES. ----
    audio = render_lap(seconds=LAP_SECONDS)
    check("the lap renders stereo", audio.shape[0] == 2, str(audio.shape))
    check("the lap is finite", bool(np.all(np.isfinite(audio))))
    check("the lap is not silent", float(np.abs(audio).max()) > 0.0)

    # Quarter-lap energy: the click starts in front, passes the left ear, the back, then the
    # right. So the left-minus-right balance must CHANGE SIGN between the two side quarters.
    # Demanded with a margin, because a broken orbit leaves both quarters near zero and a bare
    # sign comparison on two near-zero numbers is a coin flip.
    q = audio.shape[1] // 4
    left_q = audio[:, q:2 * q]          # passing the left ear
    right_q = audio[:, 3 * q:]          # passing the right ear

    def balance(block):
        el = float(np.sum(block[0].astype(np.float64) ** 2))
        er = float(np.sum(block[1].astype(np.float64) ** 2))
        return (el - er) / (el + er) if (el + er) > 0.0 else 0.0

    bl, br = balance(left_q), balance(right_q)
    check("the click swings left then right", bl > 0.05 and br < -0.05,
          "{0:+.3f} then {1:+.3f}".format(bl, br))

    print("FAILURES: {0}".format(failures) if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
