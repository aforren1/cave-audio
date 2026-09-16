"""The commit model: what lands when, asked of the RENDER rather than of the wrapper.

Commit is the engine's frame boundary. Position and pose write pending fields and only a commit
promotes them, so "did this land" is a question about audio, not about a Python flag. Every test
here that can be answered by the manual sink is answered by it: a source at room -x is rendered by
the -x half of the array, and the split moves when, and only when, a commit has happened.

The commit COUNT is the one thing no render can report, so those tests count the C calls through a
spy on `_bwa.commit`.
"""

from __future__ import annotations

import numpy as np
import pytest

import bw_audio as bwa
from conftest import BLK, SR, golden_tone_block, tone, write_wav

# Room -x is the listener's RIGHT (test/golden_test.c's own comment). Two positions far enough
# apart that the array's answer is not a judgement call: the golden scenario puts 99.6 percent of
# its energy on the near half, so a factor of 3 is a floor, not a threshold to tune.
RIGHT = (-2.0, 1.5, 0.0)
LEFT = (2.0, 1.5, 0.0)
MARGIN = 3.0


def _make_engine(**kw):
    return bwa.Engine(profile=bwa.Profile.CAVE, sample_rate=SR, block_size=BLK,
                      sink=bwa.SinkType.MANUAL, **kw)


class _Rig:
    """A started manual-sink engine, one push source, and a lateral energy readout."""

    def __init__(self, engine):
        self.e = engine
        spk = np.asarray(engine.speakers)
        self.right_ch = spk[:, 0] < -0.5
        self.left_ch = spk[:, 0] > 0.5
        self.src = engine.create_push_source()
        self.n = 0

    def bias(self, blocks=8):
        """Render `blocks` blocks of tone through the source. Returns (right energy, left energy)."""
        energy = np.zeros(self.e.channel_count)
        for _ in range(blocks):
            self.src.push(golden_tone_block(self.n))
            self.n += 1
            out = self.e.render_block()
            energy += np.sum(out.astype(np.float64) ** 2, axis=1)
        return float(energy[self.right_ch].sum()), float(energy[self.left_ch].sum())

    def assert_right(self, what, blocks=8):
        r, l = self.bias(blocks)
        assert r > l * MARGIN, "{0}: right {1:.4f} against left {2:.4f}".format(what, r, l)

    def assert_left(self, what, blocks=8):
        r, l = self.bias(blocks)
        assert l > r * MARGIN, "{0}: left {1:.4f} against right {2:.4f}".format(what, l, r)


@pytest.fixture
def rig():
    """A rig settled at RIGHT, with autocommit on. Anything after this is the test's own doing."""
    e = _make_engine()
    try:
        r = _Rig(e)
        r.src.set_pos(*RIGHT)
        e.listener.set_pose(0.0, 1.5, 0.0)
        e.start()
        r.assert_right("the rig did not settle where it was put")
        yield r
    finally:
        e.close()


@pytest.fixture
def commits(monkeypatch):
    """Every `bwa_commit` the layer makes, counted at the C call."""
    seen = []
    real = bwa._bwa.commit

    def spy(engine):
        seen.append(1)
        real(engine)

    monkeypatch.setattr(bwa._bwa, "commit", spy)
    return seen


# ---------------------------------------------------------------------------- autocommit


def test_a_set_with_no_explicit_commit_is_rendered(rig):
    """The whole point: no commit() in this test, and the source still moves."""
    rig.src.set_pos(*LEFT)
    rig.assert_left("a position set under autocommit never reached the render")


def test_a_pose_set_with_no_explicit_commit_is_rendered():
    """Turn the listener's HEAD instead. Only the headphone renders read orientation, so binaural.

    Which output channel is which ear is deliberately not asserted: a laterality check that names
    an ear is how a left/right mirror once shipped. This asserts that the bias SWAPS SIDES, which
    a mirrored decode satisfies no better than a correct one.
    """
    e = bwa.Engine(profile=bwa.Profile.BINAURAL, sample_rate=SR, block_size=BLK,
                   sink=bwa.SinkType.MANUAL)
    try:
        src = e.create_push_source()
        src.set_pos(*RIGHT)                       # lateral to a listener facing ROOM_AHEAD
        e.listener.set_pose(0.0, 1.5, 0.0)
        e.start()

        def ratio(n=0):
            energy = np.zeros(2)
            for b in range(8):
                src.push(golden_tone_block(n + b))
                energy += np.sum(e.render_block().astype(np.float64) ** 2, axis=1)
            assert energy.min() > 0.0
            return float(energy[0] / energy[1])

        facing = ratio(0)
        e.listener.set_pose(0.0, 1.5, 0.0, 0.0, 1.0, 0.0, 0.0)   # yaw half a turn, no commit
        turned = ratio(8)

        assert max(facing, 1.0 / facing) > 2.0, "no lateral bias to observe: {0:.3f}".format(facing)
        assert (facing > 1.0) != (turned > 1.0), (
            "a listener orientation set under autocommit never reached the render: the bias stayed "
            "on the same side ({0:.3f} then {1:.3f})".format(facing, turned)
        )
    finally:
        e.close()


def test_autocommit_commits_once_per_setter_outside_a_frame_block(manual_engine, commits):
    """The cost the frame block exists to remove, stated as a number rather than as a warning."""
    e = manual_engine
    a, b = e.create_push_source(), e.create_push_source()
    del commits[:]
    a.set_pos(1.0, 1.5, 0.0)
    b.set_pos(-1.0, 1.5, 0.0)
    e.listener.set_pose(0.0, 1.5, 0.0)
    assert len(commits) == 3


def test_a_setter_that_is_not_commit_gated_does_not_commit(manual_engine, commits):
    """Gain, spread, orientation and the rest land on the next block by themselves.

    Auto-committing them would be a ring write per call for nothing, so this pins the list to
    what the header actually gates.
    """
    e = manual_engine
    s = e.create_push_source()
    bed = e.create_bed()
    del commits[:]
    s.set_gain(0.5)
    s.set_spread(0.5)
    s.set_orientation(0.0, 0.0, 0.0, 1.0)
    bed.set_orientation(1.0)
    e.set_master_gain(0.9)
    assert commits == []
    assert not e.pending


# ---------------------------------------------------------------------------- frame blocks


def test_a_frame_block_defers_the_set_until_exit(rig):
    with rig.e.frame():
        rig.src.set_pos(*LEFT)
        rig.assert_right("a frame block rendered a set before its exit")
    rig.assert_left("a frame block did not commit at its exit")


def test_two_setters_in_one_frame_block_commit_once(manual_engine, commits):
    e = manual_engine
    a, b = e.create_push_source(), e.create_push_source()
    del commits[:]
    with e.frame():
        a.set_pos(1.0, 1.5, 0.0)
        b.set_pos(-1.0, 1.5, 0.0)
        e.listener.set_pose(0.0, 1.5, 0.0)
        assert commits == [], "a frame block must not commit per setter"
    assert len(commits) == 1


def test_nested_frame_blocks_commit_once_at_the_outermost_exit(manual_engine, commits):
    e = manual_engine
    a, b = e.create_push_source(), e.create_push_source()
    del commits[:]
    with e.frame():
        a.set_pos(1.0, 1.5, 0.0)
        with e.frame():
            b.set_pos(-1.0, 1.5, 0.0)
        assert commits == [], "the INNER exit committed; only the outermost may"
    assert len(commits) == 1


def test_an_exception_leaving_a_frame_block_still_commits(manual_engine, commits):
    """A half-pending scene outlives the exception and renders mixed positions from then on.

    That is worse than the exception and harder to read, so the exit commits either way. The
    exception itself must propagate unchanged.
    """
    e = manual_engine
    s = e.create_push_source()
    del commits[:]
    with pytest.raises(ValueError, match="boom"):
        with e.frame():
            s.set_pos(1.0, 1.5, 0.0)
            raise ValueError("boom")
    assert len(commits) == 1
    assert not e.pending


def test_an_explicit_commit_inside_a_frame_block_leaves_nothing_for_the_exit(manual_engine, commits):
    e = manual_engine
    s = e.create_push_source()
    del commits[:]
    with e.frame():
        s.set_pos(1.0, 1.5, 0.0)
        e.commit()
        assert len(commits) == 1
    assert len(commits) == 1, "the exit committed again over an explicit commit"


def test_a_frame_block_with_no_writes_commits_nothing(manual_engine, commits):
    e = manual_engine
    del commits[:]
    with e.frame():
        pass
    assert commits == []


# ---------------------------------------------------------------------------- the play flush


def test_play_inside_a_frame_block_renders_at_the_new_position(manual_engine, tmp_path):
    """Set the position, play it: the voice must sound where you put it, from its first block.

    The render happens INSIDE the block on purpose. With a live device the audio thread renders
    between the play and the block's exit, and a first block at the old position is exactly the
    defect the flush exists to prevent.
    """
    e = manual_engine
    snd = e.load_sound(write_wav(tmp_path / "steady.wav", tone(SR // 2, amp=0.5)))
    spk = np.asarray(e.speakers)
    right_ch, left_ch = spk[:, 0] < -0.5, spk[:, 0] > 0.5

    src = e.create_source()
    src.set_pos(*RIGHT)
    e.listener.set_pose(0.0, 1.5, 0.0)

    with e.frame():
        src.set_pos(*LEFT)
        src.play(snd)               # flushes the pending position first
        energy = np.zeros(e.channel_count)
        for _ in range(6):
            energy += np.sum(e.render_block().astype(np.float64) ** 2, axis=1)

    r, l = float(energy[right_ch].sum()), float(energy[left_ch].sum())
    assert l > r * MARGIN, "play did not flush: the voice started at the old position ({0:.4f} " \
                           "left against {1:.4f} right)".format(l, r)
    snd.unload()


def test_a_play_with_nothing_pending_commits_nothing(manual_engine, commits, tmp_path):
    """The flush is conditional. A play in a loop must not become a commit in a loop."""
    e = manual_engine
    snd = e.load_sound(write_wav(tmp_path / "steady.wav", tone(SR // 8, amp=0.5)))
    src = e.create_source()
    src.set_pos(*RIGHT)
    del commits[:]
    src.play(snd)
    assert commits == []
    snd.unload()


# ---------------------------------------------------------------------------- autocommit off


def test_autocommit_off_restores_the_c_semantics():
    """Nothing commits but commit(), which is what the raw layer does and what a frame loop wants."""
    e = _make_engine(autocommit=False)
    try:
        r = _Rig(e)
        r.src.set_pos(*RIGHT)
        e.listener.set_pose(0.0, 1.5, 0.0)
        e.commit()
        e.start()
        r.assert_right("the rig did not settle where it was put")

        r.src.set_pos(*LEFT)
        assert e.pending
        r.assert_right("autocommit=False committed a position it was told not to")

        e.commit()
        assert not e.pending
        r.assert_left("an explicit commit did not land with autocommit off")
    finally:
        e.close()


def test_autocommit_off_means_a_play_does_not_flush_either(manual_engine, commits, tmp_path):
    e = manual_engine
    e.autocommit = False
    snd = e.load_sound(write_wav(tmp_path / "steady.wav", tone(SR // 8, amp=0.5)))
    src = e.create_source()
    del commits[:]
    src.set_pos(*RIGHT)
    src.play(snd)
    assert commits == []
    assert e.pending
    snd.unload()


def test_autocommit_off_means_a_frame_block_does_not_commit_at_exit(manual_engine, commits):
    e = manual_engine
    e.autocommit = False
    s = e.create_push_source()
    del commits[:]
    with e.frame():
        s.set_pos(1.0, 1.5, 0.0)
    assert commits == [], "autocommit off must mean nothing commits for you, exits included"
    assert e.pending


# ---------------------------------------------------------------------------- the raw layer


def test_the_raw_layer_does_not_auto_commit(rig):
    """Documented: the raw layer is the ABI and nothing else. Same engine, same source, no commit."""
    e = rig.e
    bwa._bwa.source_set_pos(e.raw, rig.src.handle, *LEFT)
    rig.assert_right("the raw layer committed for the caller")
    bwa._bwa.commit(e.raw)
    rig.assert_left("an explicit raw commit did not land")


def test_the_raw_layer_does_not_touch_the_wrappers_pending_flag(manual_engine):
    e = manual_engine
    s = e.create_push_source()
    bwa._bwa.source_set_pos(e.raw, s.handle, 1.0, 1.5, 0.0)
    assert not e.pending, "the flag tracks what the Pythonic layer wrote, not what the ABI holds"
