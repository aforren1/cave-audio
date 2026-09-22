#!/usr/bin/env python
"""Offline experiment shape: render a moving source to a numpy array and write a wav. No device.

This is the shape to start with for any experiment that can pre-render its stimuli
(docs/backends.md, "Experiments from Psychtoolbox or PsychoPy"). The manual sink creates no device
and no audio thread: you pump one block at a time on your own thread, the clock is a pure sample
counter, and a fixed input with a fixed call sequence renders bit-identically every run. PsychoPy
or PsychPortAudio then plays the file with its own sample-accurate scheduler.

Positions are commit-gated in the engine, and this binding commits them for you, so the render
loop below is just a set and a push. Wrap several writes in `with e.frame():` when they must land
as one snapshot (README, "Commit model").

Run it:      uv run examples/offline_render.py
Check it:    uv run examples/offline_render.py --tests
"""

from __future__ import annotations

import argparse
import math
import sys
import wave

import numpy as np

import bw_audio as bwa
import stimulus

SR = 48000
BLK = 256
SECONDS = 2.0


def render(profile, seconds=SECONDS, sample_rate=SR, block_size=BLK):
    """Render the shared click orbiting the listener. Returns (channels, frames) float32.

    Same stimulus as `minimal.py` and `examples/minimal.c`, so a rendered file and a live run
    are comparable by ear.
    """
    e = bwa.Engine(profile=profile, sample_rate=sample_rate, block_size=block_size,
                   sink=bwa.SinkType.MANUAL)
    try:
        src = e.create_push_source()
        src.set_gain(0.5)
        e.listener.set_pose(0.0, 1.5, 0.0)   # commit-gated, and this layer commits it for you
        e.start()

        period = stimulus.click_period(sample_rate)
        nblocks = int(round(seconds * sample_rate / block_size))
        out = []
        cursor = 0
        for b in range(nblocks):
            t0 = b * block_size

            # Move the source: one full turn over the render, at 2 m out and head height.
            angle = 2.0 * math.pi * (t0 / (seconds * sample_rate))
            src.set_pos(2.0 * math.sin(angle), 1.5, 2.0 * math.cos(angle))

            # Feed the voice one block of the click period, wrapping, then pull one block out.
            take = min(block_size, period.size - cursor)
            src.push(period[cursor:cursor + take])
            if take < block_size:
                src.push(period[:block_size - take])
            cursor = (cursor + block_size) % period.size

            block = e.render_block()
            out.append(np.array(block, copy=True))   # the view dies at the next render_block

        return np.concatenate(out, axis=1)
    finally:
        e.close()


def write_wav(path, planar, sample_rate=SR):
    """Write a (channels, frames) float32 array as interleaved 16-bit PCM, with numpy alone."""
    interleaved = np.ascontiguousarray(planar.T)
    ints = (np.clip(interleaved, -1.0, 1.0) * 32767.0).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(planar.shape[0])
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(ints.tobytes())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--profile", choices=["cave", "binaural"], default="binaural",
                    help="binaural renders 2 channels for headphones; cave renders the array")
    ap.add_argument("--out", default="orbit.wav")
    ap.add_argument("--tests", action="store_true",
                    help="self-check mode: render both profiles, assert, write nothing, exit nonzero on a miss")
    args = ap.parse_args(argv)

    if args.tests:
        return selftest()

    profile = bwa.Profile.BINAURAL if args.profile == "binaural" else bwa.Profile.CAVE
    audio = render(profile)
    write_wav(args.out, audio)
    print("rendered {0} channels x {1} frames ({2:.2f} s), peak {3:.4f} -> {4}".format(
        audio.shape[0], audio.shape[1], audio.shape[1] / SR, float(np.abs(audio).max()), args.out))
    return 0


def selftest():
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        print("{0:<44} {1}{2}".format(name, "ok" if ok else "FAIL", ("  " + detail) if detail else ""))
        if not ok:
            failures += 1

    binaural = render(bwa.Profile.BINAURAL, seconds=0.25)
    check("binaural render is stereo", binaural.shape[0] == 2, str(binaural.shape))
    # A render is whole BLOCKS, so the length rounds to the block size rather than to the second.
    check("binaural render is a whole number of blocks", binaural.shape[1] % BLK == 0,
          str(binaural.shape[1]))
    check("binaural render is within a block of the request",
          abs(binaural.shape[1] - 0.25 * SR) <= BLK, str(binaural.shape[1]))
    check("binaural render is finite", bool(np.all(np.isfinite(binaural))))
    check("binaural render is not silent", float(np.abs(binaural).max()) > 0.0)

    # One second is one lap AND four click periods, so a burst lands on each quarter turn.
    cave = render(bwa.Profile.CAVE, seconds=1.0)
    check("cave render is the array width", cave.shape[0] >= 4, str(cave.shape))
    check("cave render is not silent", float(np.abs(cave).max()) > 0.0)

    # The whole reason this shape exists: the same calls twice give the same samples.
    again = render(bwa.Profile.CAVE, seconds=1.0)
    check("the offline render is bit-identical run to run", np.array_equal(cave, again))

    # A moving source must actually move, or the auto-commit is not doing its job. Compare the
    # burst a quarter of the way round against the one three quarters round: opposite sides of
    # the listener, so the loudest speaker cannot be the same one. Comparing the FIRST and LAST
    # blocks instead would pass on a source that never moved, because both would be silence.
    def loudest_at(t_seconds):
        i = int(t_seconds * SR)
        window = cave[:, i:i + SR // 100].astype(np.float64)
        return int(np.argmax(np.sum(window * window, axis=1)))

    check("the source moved across the render", loudest_at(0.25) != loudest_at(0.75),
          "speaker {0} against {1}".format(loudest_at(0.25), loudest_at(0.75)))

    print("FAILURES: {0}".format(failures) if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
