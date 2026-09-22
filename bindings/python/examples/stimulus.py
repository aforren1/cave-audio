#!/usr/bin/env python
"""The click every binding's minimal example plays, spelled out once.

One 250 ms period at 48 kHz: a 2 ms Hann-windowed noise burst at -12 dBFS peak, then silence.
Looped, that is four clicks a second, and an orbit reads as a trajectory rather than as a level
pan. A tone would not do: the cues that place a source in elevation and front-back live in the
SPECTRUM, and a narrowband tone carries almost none of them.

The noise comes from a 32-bit LCG written out here rather than from ``random`` or
``numpy.random``, because every language's copy of this stimulus has to produce the same samples
and no two languages' generators agree. ``examples/minimal.c`` is the reference; the Godot, MATLAB
and Unity copies follow it call for call.
"""

from __future__ import annotations

import math

import numpy as np

RATE = 48000
PERIOD_SECONDS = 0.25     # four clicks a second
BURST_SECONDS = 0.002     # the burst itself
PEAK = 0.251189           # -12 dBFS


def click_period(sample_rate: int = RATE) -> np.ndarray:
    """One period of the stimulus as mono float32: the burst, then silence."""
    period = int(round(PERIOD_SECONDS * sample_rate))
    nburst = int(round(BURST_SECONDS * sample_rate))

    burst = np.empty(nburst, dtype=np.float64)
    state = 12345
    for i in range(nburst):
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        u = ((state >> 8) & 0xFFFFFF) / 8388608.0 - 1.0          # [-1, 1)
        w = 0.5 - 0.5 * math.cos(2.0 * math.pi * i / (nburst - 1))
        burst[i] = w * u
    burst *= PEAK / np.abs(burst).max()                          # exactly -12 dBFS at the crest

    out = np.zeros(period, dtype=np.float32)
    out[:nburst] = burst.astype(np.float32)
    return out
