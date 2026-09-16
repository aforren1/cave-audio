#!/usr/bin/env python
"""Live experiment shape: land a sound on a visual event, to the sample.

The engine owns the device and the experiment schedules onsets ahead of time. Wall time and the
dsp-sample clock are different clocks, so the recipe is docs/api.md's "Land a sound on a visual
event": map your wall time to a dsp sample with the driver-stamped pair (`Engine.clock`), subtract
the device's render-to-DAC delay (`Engine.output_latency_frames`), and schedule with
`Source.play_at`.

The one thing the engine cannot see is YOUR display delay. Measure draw-to-photons once, with a
photodiode or an AV-sync clapper, and that single constant aligns the whole chain.

Run it:      uv run examples/live_onset.py
Check it:    uv run examples/live_onset.py --tests
"""

from __future__ import annotations

import argparse
import math
import sys
import time

import numpy as np

import bw_audio as bwa

SR = 48000
BLK = 256
DISPLAY_SECONDS = 0.030   # draw -> photons. Yours to measure; this is a plausible placeholder.


class WallToDsp:
    """The wall-clock to dsp-sample bridge from docs/api.md, in Python.

    The driver-stamped (sample, host time) pair is exact, so the only thing to estimate is the
    constant epoch offset between the device's host clock and `time.perf_counter()`. Each refresh
    observes one candidate offset; a decaying max converges on the true one and tracks ppm drift,
    which is what makes this hold for a two-hour session instead of only near the anchor.
    """

    def __init__(self, engine):
        self.e = engine
        self.valid = False
        self.sample = 0
        self.host = 0.0
        self.off = 0.0

    def refresh(self):
        """Call once per frame, before any `at()`."""
        pair = self.e.clock
        if pair is None:
            self.valid = False
            return
        cs, ct = pair
        host = ct * 1e-9
        cand = host - time.perf_counter()
        if not self.valid or cs < self.sample or abs(cand - self.off) > 0.5:
            self.off = cand                      # first pair, a device restart, an epoch change
        else:
            self.off = max(cand, self.off - 2e-6)  # decaying max
        self.sample, self.host, self.valid = cs, host, True

    def at(self, wall_seconds):
        """A `time.perf_counter()` reading, as a dsp sample."""
        fs = float(self.e.sample_rate)
        if self.valid:
            d = self.sample + (wall_seconds + self.off - self.host) * fs
        else:
            # Before the first stamped block: pair the block counter with your own clock. That is
            # block-granular, about 5 ms at 256/48 kHz, already under half a 60 Hz frame.
            d = self.e.dsp_time_frames + (wall_seconds - time.perf_counter()) * fs
        return max(0, int(d))


def make_click():
    """A short decaying click, pushed rather than loaded, so the example needs no asset."""
    n = SR // 10
    i = np.arange(n, dtype=np.float64)
    env = np.exp(-i / (SR * 0.01))
    return (0.6 * env * np.sin(2.0 * math.pi * 1000.0 * i / SR)).astype(np.float32)


def run(sink, profile, trials, lead_seconds, device=None, verbose=True):
    """Schedule `trials` clicks, each landing one `lead_seconds` after the decision to play it.

    Returns the list of (scheduled dsp sample, dsp sample at the decision) pairs.
    """
    e = bwa.Engine(profile=profile, sample_rate=SR, block_size=BLK, sink=sink, device=device)
    scheduled = []
    try:
        e.start()
        if verbose:
            print("backend: {0}".format(e.backend))
            print("output latency: {0} frames ({1:.1f} ms)".format(
                e.output_latency_frames, e.output_latency_seconds * 1e3))
            h = e.health
            print("dropout counters: {0}".format(
                "measurable" if h is not None else "NOT measurable on this sink"))

        click = make_click()
        src = e.create_push_source()
        src.set_pos(0.0, 1.5, 2.0)
        e.commit()

        clock = WallToDsp(e)
        for trial in range(trials):
            clock.refresh()

            # The visual event: drawn now, seen one display delay later.
            t_seen = time.perf_counter() + lead_seconds + DISPLAY_SECONDS
            heard = clock.at(t_seen)
            latency = e.output_latency_frames
            start = heard - latency if heard > latency else 0

            # A push source has no play call, so this trial pushes its samples and the ENGINE
            # consumes them as its clock reaches them. For a file asset the whole trial is
            # `src.play_at(sound, start)` instead, which is the sample-accurate form.
            src.push(click)
            scheduled.append((start, e.dsp_time_frames))
            if verbose:
                print("trial {0}: scheduled for dsp sample {1} (now {2})".format(
                    trial, start, e.dsp_time_frames))

            e.commit()
            deadline = time.perf_counter() + lead_seconds + 0.05
            while time.perf_counter() < deadline:
                time.sleep(0.005)

        if verbose:
            h = e.health
            if h is not None:
                print("xruns {0} in {1} blocks, peak load {2:.2f}".format(h.xruns, h.blocks, h.peak_load))
    finally:
        e.close()
    return scheduled


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--trials", type=int, default=5)
    ap.add_argument("--lead", type=float, default=0.5, help="seconds from decision to onset")
    ap.add_argument("--device", default=None, help="exact device name or id; see --list-devices")
    ap.add_argument("--list-devices", action="store_true")
    ap.add_argument("--tests", action="store_true",
                    help="self-check mode: force the offline sink, assert, exit nonzero on a miss")
    args = ap.parse_args(argv)

    if args.list_devices:
        for backend in (bwa.SinkType.WASAPI, bwa.SinkType.ASIO, bwa.SinkType.JACK,
                        bwa.SinkType.ALSA, bwa.SinkType.COREAUDIO):
            for i, (name, ident) in enumerate(bwa.list_devices(backend)):
                print("{0!s:<22} {1}: {2}  [{3}]".format(backend, i, name, ident))
        return 0

    if args.tests:
        return selftest()

    # AUTO picks by channel count and platform and falls back to the silent offline sink, so this
    # runs with or without headphones plugged in. Name a backend to demand one instead.
    run(bwa.SinkType.AUTO, bwa.Profile.BINAURAL, args.trials, args.lead, device=args.device)
    return 0


def selftest():
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        print("{0:<48} {1}{2}".format(name, "ok" if ok else "FAIL", ("  " + detail) if detail else ""))
        if not ok:
            failures += 1

    # The null sink is paced from the host clock with no device, so the whole scheduling path runs.
    scheduled = run(bwa.SinkType.NULL, bwa.Profile.BINAURAL, trials=3, lead_seconds=0.15,
                    verbose=False)
    check("every trial produced a schedule", len(scheduled) == 3)
    check("every scheduled onset is in the future", all(s > now for s, now in scheduled),
          str(scheduled))
    check("onsets advance across trials", all(
        scheduled[i][0] < scheduled[i + 1][0] for i in range(len(scheduled) - 1)))

    lead_frames = [s - now for s, now in scheduled]
    want = int((0.15 + DISPLAY_SECONDS) * SR)
    # Generous: the null sink is host-paced and this asserts the ARITHMETIC lands in the right
    # decade, not the jitter of a sleeping Python loop.
    check("the lead is about what was asked for",
          all(0.5 * want <= f <= 2.0 * want for f in lead_frames),
          "{0} against {1}".format(lead_frames, want))

    with bwa.Engine(sink=bwa.SinkType.NULL, sample_rate=SR, block_size=BLK) as e:
        c = WallToDsp(e)
        c.refresh()
        # Without a stamped pair the bridge must still answer, block-granular, rather than fail.
        check("the bridge answers before the first stamped block", c.at(time.perf_counter()) >= 0)
        check("a past wall time clamps at 0", c.at(time.perf_counter() - 1e6) == 0)

    print("FAILURES: {0}".format(failures) if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
