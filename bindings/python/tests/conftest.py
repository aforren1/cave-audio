"""Shared fixtures. Nothing here touches a real device: every engine is the null or manual sink."""

from __future__ import annotations

import math
import struct
import wave

import numpy as np
import pytest

import bw_audio as bwa

SR = 48000
BLK = 256


@pytest.fixture
def manual_engine():
    """A started manual-sink engine at the golden test's rate and block size."""
    e = bwa.Engine(profile=bwa.Profile.CAVE, sample_rate=SR, block_size=BLK, sink=bwa.SinkType.MANUAL)
    e.start()
    try:
        yield e
    finally:
        e.close()


@pytest.fixture
def null_engine():
    """A CREATED (not started) null-sink engine. Start it in the test that wants it running."""
    e = bwa.Engine(profile=bwa.Profile.CAVE, sample_rate=SR, block_size=BLK, sink=bwa.SinkType.NULL)
    try:
        yield e
    finally:
        e.close()


def write_wav(path, samples, sample_rate=SR):
    """Write mono 16-bit PCM with the stdlib, so the test depends on no audio package.

    Returns the path as a str, which is what the ABI takes.
    """
    pcm = np.clip(np.asarray(samples, dtype=np.float64), -1.0, 1.0)
    ints = (pcm * 32767.0).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(ints.tobytes())
    return str(path)


def tone(n, freq=440.0, amp=0.25, start=0, sample_rate=SR):
    """A continuous-phase sine as float32, phase anchored at absolute frame `start`."""
    i = np.arange(start, start + n, dtype=np.float64)
    return (amp * np.sin(2.0 * math.pi * freq * i / sample_rate)).astype(np.float32)


def golden_tone_block(block_index, n=BLK):
    """Exactly test/golden_test.c's fill_tone, in float32 to match its sinf arithmetic."""
    start = block_index * n
    i = np.arange(start, start + n, dtype=np.float32)
    w = np.float32(6.2831853) * np.float32(440.0)
    return (np.float32(0.25) * np.sin((w * i / np.float32(SR)).astype(np.float32))).astype(np.float32)


__all__ = ["SR", "BLK", "write_wav", "tone", "golden_tone_block", "struct"]
