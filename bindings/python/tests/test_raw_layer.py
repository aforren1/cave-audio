"""The raw layer: what it covers, what it deliberately does not, and the struct round trips."""

from __future__ import annotations

import os
import re

import numpy as np
import pytest

import bw_audio as bwa
from bw_audio import _bwa

HEADER = os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "include", "bw_audio.h")
)

# The one ABI call this binding refuses to expose, and the reason. bwa_set_output_capture installs
# a callback that the engine invokes ON THE AUDIO THREAD; taking the GIL there would allocate,
# block on a lock and run bytecode inside the device callback, which is exactly what CLAUDE.md's
# first invariant forbids and what docs/backends.md rules out for this binding by name.
EXCLUDED = {"bwa_set_output_capture"}


def test_the_audio_thread_callback_is_not_bound():
    assert not hasattr(_bwa, "set_output_capture")
    assert not any("capture" in n for n in dir(_bwa))
    assert not any("capture" in n for n in dir(bwa))


def test_every_other_abi_call_is_bound():
    """Parse the header and demand a binding for every BWA_API entry point but the excluded one.

    This is the guard that keeps the raw layer 1:1 as the ABI grows: a new call arrives red here
    rather than quietly missing.
    """
    if not os.path.exists(HEADER):
        pytest.skip("include/bw_audio.h is not beside this test (running outside a repo checkout)")

    with open(HEADER, encoding="utf-8") as f:
        text = f.read()
    names = set(re.findall(r"^BWA_API[^;]*?\b(bwa_[a-z0-9_]+)\s*\(", text, re.MULTILINE | re.DOTALL))
    assert len(names) > 150, "the header parse found only {0} entry points".format(len(names))

    missing = sorted(n for n in names - EXCLUDED if not hasattr(_bwa, n[len("bwa_") :]))
    assert not missing, "unbound ABI calls: {0}".format(missing)


def test_enum_values_match_the_abi():
    assert int(bwa.Profile.CAVE) == 0
    assert int(bwa.Profile.CAVE_BOTH) == 3
    assert int(bwa.SinkType.MANUAL) == 3
    assert int(bwa.Result.ERR_LAYOUT) == 3
    assert int(bwa.Panner.DBAP) == 0
    assert int(bwa.LoadFlags.AMBIX) == 2
    assert bwa.CHANNEL_AUTO == -1
    assert bwa.GROUPS == 8
    assert bwa.MAX_CHANNELS == 64
    assert bwa.DEFAULT_GRID == 26


def test_room_basis_is_the_headers():
    assert bwa.ROOM_AHEAD == (0.0, 0.0, 1.0)
    assert bwa.ROOM_UP == (0.0, 1.0, 0.0)
    assert bwa.ROOM_RIGHT == (-1.0, 0.0, 0.0)


def test_source_desc_round_trips(manual_engine):
    e = manual_engine
    d = bwa.source_preset(bwa.SourceKind.AMBIENCE)
    assert d.struct_size > 0
    src = e.create_source(d)
    got = src.get_desc()
    assert got is not None
    assert got.spread == pytest.approx(d.spread)
    assert got.gain == pytest.approx(d.gain)

    # preset -> override -> apply composes, which is the whole reason for the fill-then-apply shape.
    d.gain = 0.5
    assert src.apply(d)
    assert src.get_desc().gain == pytest.approx(0.5)


def test_a_zero_filled_source_desc_is_refused(manual_engine):
    """Its zero is NOT its default, so a zero-init mistake has to fail loudly."""
    d = _bwa.source_desc()
    assert d.struct_size == 0
    src = manual_engine.create_source()
    assert not src.apply(d)
    assert manual_engine.last_error


def test_presets_differ_from_the_default():
    """A preset that matched BWA_SRC_DEFAULT field for field would make its test unfalsifiable."""
    default = bwa.source_preset(bwa.SourceKind.DEFAULT)
    ui = bwa.source_preset(bwa.SourceKind.UI)
    fields = ("gain", "spread", "atten_ref_dist", "atten_rolloff", "reverb", "doppler")
    assert any(getattr(default, f) != getattr(ui, f) for f in fields)


def test_tuning_round_trips(manual_engine):
    t = bwa.tuning_preset(bwa.Setup.SEATED)
    assert t.struct_size > 0
    assert manual_engine.apply_tuning(t)
    back = manual_engine.get_tuning()
    assert back.panner == t.panner
    assert back.max_re == t.max_re


def test_pure_solves_need_no_engine():
    """panner_gains and bed_gains are reentrant and engine-free: a tool can score a layout offline."""
    positions = np.array(
        [[1, 0, 0], [-1, 0, 0], [0, 0, 1], [0, 0, -1], [0, 1, 0], [0, -1, 0]], dtype=np.float32
    )
    srcs = np.array([[1, 0, 0], [0, 0, 1]], dtype=np.float32)
    g = bwa.panner_gains(bwa.Panner.DBAP, positions, (0.0, 0.0, 0.0), srcs)
    assert g.shape == (2, 6)
    assert np.all(np.isfinite(g))
    assert g[0].argmax() == 0, "a source at +x should load the +x speaker hardest"

    dirs = np.array([[0, 0, 1]], dtype=np.float32)
    b = bwa.bed_gains(bwa.BedDecoder.ALLRAD, True, positions, dirs)
    assert b.shape == (1, 6)
    assert np.all(np.isfinite(b))

    assert bwa.spcap_focus_default(positions) > 0.0


def test_box_mesh_is_pure_and_bounded():
    mesh = bwa.box_mesh(4.0, 3.0, 5.0, [0, 0, 0, 0, 0, 0])
    assert mesh is not None
    verts, tris, mats = mesh
    assert verts.shape == (8, 3) and tris.shape == (12, 3) and mats.shape == (12,)
    assert bwa.box_mesh(-1.0, 3.0, 5.0, [0] * 6) is None, "an illegal box reports by returning None"


def test_the_engine_handle_is_not_destroyed_by_the_collector():
    """bwa_destroy is a control-thread call; the collector runs wherever it likes, so the wrapper
    must not call it. Dropping the last reference leaks rather than crashing, and destroy() is how
    you free it."""
    e = bwa.Engine(sink=bwa.SinkType.NULL)
    raw = e.raw
    del e
    import gc

    gc.collect()
    assert raw.valid
    _bwa.destroy(raw)
    assert not raw.valid


def test_sink_flags_mirror_the_abi_bits():
    """sink_flags are #defines in the ABI, so the raw layer carries constants and this layer an
    IntFlag. Combining them has to stay a plain OR the desc accepts."""
    assert int(bwa.SinkFlags.EXCLUSIVE) == 0x1
    assert int(bwa.SinkFlags.EXACT_RATE) == 0x2
    assert int(bwa.SinkFlags.TIGHT_BUFFER) == 0x4
    assert int(bwa.SinkFlags.EXCLUSIVE | bwa.SinkFlags.EXACT_RATE) == 0x3

    d = _bwa.desc()
    d.sink_flags = int(bwa.SinkFlags.EXACT_RATE | bwa.SinkFlags.TIGHT_BUFFER)
    assert d.sink_flags == 0x6

    # They describe the PRIMARY device, and an offline sink has none - so they are inert rather
    # than an error, which is what lets an experiment carry one configuration to both shapes.
    with bwa.Engine(sink=bwa.SinkType.NULL, sink_flags=bwa.SinkFlags.EXCLUSIVE) as e:
        assert e.backend == "null"
