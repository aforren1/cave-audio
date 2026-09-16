"""Lifecycle on the offline sinks: create, start, the resolved config, health, stop, destroy."""

from __future__ import annotations

import pytest

import bw_audio as bwa
from bw_audio import _bwa


def test_abi_matches_header():
    """The library and the extension must agree on major.minor, or nothing below means anything."""
    bwa.check_abi()
    assert bwa.abi_version()[:2] == bwa.header_abi_version()[:2]


def test_null_sink_lifecycle(null_engine):
    e = null_engine
    assert e.sample_rate == 48000
    assert e.block_size == 256
    assert 4 <= e.channel_count <= 26

    # Before start, get_sink_type reports the POLICY, which is what we asked for.
    assert e.sink_type == bwa.SinkType.NULL
    assert e.backend == "none"

    e.start()
    assert e.sink_type == bwa.SinkType.NULL
    assert e.backend == "null"
    assert e.last_error is None

    e.stop()
    assert e.backend == "none"


def test_health_measured_semantics(manual_engine):
    """The manual sink has no clock and no deadline, so it cannot miss one - and must SAY so.

    None here is not a clean bill of health, it is "never measured". A zero xrun count on its own
    is indistinguishable from the two.
    """
    assert manual_engine.health is None
    assert manual_engine.xruns == 0


def test_health_on_the_null_sink(null_engine):
    """The null sink IS paced, so its counters mean something once it runs."""
    null_engine.start()
    h = null_engine.health
    assert h is not None
    assert h.xruns == 0 and h.dropped_frames == 0
    assert h.device_lost == 0
    assert isinstance(h.peak_load, float)


def test_context_manager_starts_and_destroys():
    with bwa.Engine(sink=bwa.SinkType.NULL, sample_rate=48000, block_size=256) as e:
        assert e.backend == "null"
        raw = e.raw
        assert raw.valid
    assert not raw.valid
    assert "destroyed" in repr(e)


def test_calls_after_destroy_raise():
    e = bwa.Engine(sink=bwa.SinkType.NULL)
    e.close()
    with pytest.raises(RuntimeError, match="destroyed"):
        e.channel_count
    e.close()  # idempotent


def test_bad_layout_path_fails_start():
    """A failed EXPLICIT layout load leaves create usable on the default grid and fails start."""
    e = bwa.Engine(sink=bwa.SinkType.NULL, layout_path="no/such/layout.json")
    try:
        assert e.last_error
        with pytest.raises(bwa.BwaError) as exc:
            e.start()
        assert exc.value.result == bwa.Result.ERR_LAYOUT
    finally:
        e.close()


def test_manual_sink_creates_no_thread(manual_engine):
    assert manual_engine.sink_type == bwa.SinkType.MANUAL
    assert manual_engine.backend == "manual"
    # Nothing renders until you pump, so the clock has not moved.
    assert manual_engine.dsp_time_frames == 0


def test_resolved_config_is_not_the_requested_one():
    """Zero fields resolve: derive seconds from the ENGINE, never from the desc you passed."""
    with bwa.Engine(sink=bwa.SinkType.NULL) as e:
        assert e.sample_rate == 48000
        assert e.block_size == 256


def test_speakers_and_channel_count_agree(null_engine):
    spk = null_engine.speakers
    assert spk.shape == (null_engine.channel_count, 3)
    assert spk.dtype.name == "float32"


def test_raw_and_pythonic_layers_interoperate(null_engine):
    """The Pythonic layer is a thin wrapper, so the raw layer can drive the same engine."""
    assert _bwa.get_channel_count(null_engine.raw) == null_engine.channel_count
