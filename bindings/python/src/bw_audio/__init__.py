"""Python binding for the bw_audio spatial audio engine.

Two layers, and you can mix them freely:

* ``bw_audio._bwa`` is the raw layer. One function per C entry point, named by its C name minus
  the ``bwa_`` prefix, with the header's own argument names and units. Nothing is renamed or
  re-unitted. Read ``include/bw_audio.h`` or ``docs/api.md`` and you are reading this layer.
* ``bw_audio`` is this module: an ``Engine`` context manager and small handle objects over it.
  It adds ergonomics and one safety guard, and no semantics of its own.

The threading contract is the engine's, not Python's: every call must come from ONE control
thread. ``Engine`` records the thread that created it and raises from any other, unless you pass
``thread_guard=False``. The raw layer has no guard.

``bwa_set_output_capture`` is deliberately not bound. Its callback runs on the audio thread, where
an interpreter must never run. Use the manual sink and ``Engine.render_block()`` instead.
"""

from __future__ import annotations

import enum
import os
import sys
import threading
from typing import TYPE_CHECKING, Optional, Tuple

if sys.platform == "win32":
    # Windows does not search a .pyd's own directory for its dependent DLLs, so the engine library
    # sitting beside the extension would not be found. This has to happen before the import below.
    _here = os.path.dirname(os.path.abspath(__file__))
    if hasattr(os, "add_dll_directory") and os.path.isdir(_here):
        try:
            _dll_dir = os.add_dll_directory(_here)
        except OSError:  # pragma: no cover - a read-only or unusual install
            _dll_dir = None

from . import _bwa  # noqa: E402

if TYPE_CHECKING:  # pragma: no cover
    import numpy as np

__all__ = [
    "BwaError",
    "Engine",
    "Sound",
    "Source",
    "PushSource",
    "Bed",
    "Listener",
    "Profile",
    "SinkType",
    "Result",
    "Panner",
    "SpreadMode",
    "BedRenderer",
    "BedDecoder",
    "Setup",
    "SourceKind",
    "TestKind",
    "Directivity",
    "MaterialType",
    "TrackerState",
    "LoadFlags",
    "SinkFlags",
    "CHANNEL_AUTO",
    "GROUPS",
    "EXTRA_LIS",
    "SINK_FLAG_EXCLUSIVE",
    "ROOM_AHEAD",
    "ROOM_UP",
    "ROOM_RIGHT",
    "list_devices",
    "abi_version",
    "header_abi_version",
    "check_abi",
    "panner_gains",
    "bed_gains",
    "spcap_focus_default",
    "source_preset",
    "tuning_preset",
    "box_mesh",
    "_bwa",
]

# ---------------------------------------------------------------------------- re-exports

Profile = _bwa.profile
SinkType = _bwa.sink_type
Result = _bwa.result
Panner = _bwa.panner
SpreadMode = _bwa.spread_mode
BedRenderer = _bwa.bed_renderer
BedDecoder = _bwa.bed_decoder
Setup = _bwa.setup
SourceKind = _bwa.source_kind
TestKind = _bwa.test_kind
Directivity = _bwa.directivity
MaterialType = _bwa.material_type
TrackerState = _bwa.tracker_state
LoadFlags = _bwa.load_flags

class SinkFlags(enum.IntFlag):
    """``Engine(sink_flags=...)`` bits, for the PRIMARY device only.

    All three are opt-in and all three trade something a shared, resampling, comfortably buffered
    device gives you for free. None of them reaches the ``CAVE_BOTH`` monitor, which opens the
    platform default shared and unflagged. ``docs/api.md`` maps them onto PsychPortAudio's latency
    classes if you are porting a psychophysics rig.

    * ``EXCLUSIVE`` takes the endpoint away from every other application on the machine. Lowest
      latency and a fixed callback size, and on WASAPI the only way past 2 channels.
    * ``EXACT_RATE`` refuses a device that cannot run at the rate you asked for, rather than
      accepting the OS resampler. Use it when a measured rate is part of the experiment.
    * ``TIGHT_BUFFER`` asks for the smallest device buffer the backend can take. Expect underruns
      on a loaded machine: it is for a quiet rig, not a default.
    """

    NONE = 0
    EXCLUSIVE = _bwa.SINK_FLAG_EXCLUSIVE
    EXACT_RATE = getattr(_bwa, "SINK_FLAG_EXACT_RATE", 0x2)
    TIGHT_BUFFER = getattr(_bwa, "SINK_FLAG_TIGHT_BUFFER", 0x4)


CHANNEL_AUTO = _bwa.CHANNEL_AUTO
GROUPS = _bwa.GROUPS
EXTRA_LIS = _bwa.EXTRA_LIS
SINK_FLAG_EXCLUSIVE = _bwa.SINK_FLAG_EXCLUSIVE
ROOM_AHEAD = _bwa.ROOM_AHEAD
ROOM_UP = _bwa.ROOM_UP
ROOM_RIGHT = _bwa.ROOM_RIGHT

source_preset = _bwa.source_preset
tuning_preset = _bwa.tuning_preset
spcap_focus_default = _bwa.spcap_focus_default
box_mesh = _bwa.box_mesh


def _np():
    import numpy

    return numpy


# ---------------------------------------------------------------------------- errors


class BwaError(RuntimeError):
    """A call the engine refused.

    ``result`` is the ``bwa_result`` code when the call reports one, and None when the call
    reports only through ``bwa_last_error``.
    """

    def __init__(self, message: str, result: Optional[int] = None):
        super().__init__(message)
        self.result = result


def _check(engine, result, what: str) -> None:
    if result == Result.OK:
        return
    reason = _bwa.last_error(engine) or "no reason reported"
    raise BwaError("{0} failed ({1!s}): {2}".format(what, result, reason), result)


# ---------------------------------------------------------------------------- version


def abi_version() -> Tuple[int, int, int]:
    """The ABI version the loaded engine library reports (``bwa_get_version``)."""
    v = _bwa.get_version()
    return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


def header_abi_version() -> Tuple[int, int, int]:
    """The ABI version the extension was compiled against."""
    return (_bwa.VERSION_MAJOR, _bwa.VERSION_MINOR, _bwa.VERSION_PATCH)


def check_abi() -> None:
    """Raise when the loaded library's major.minor differs from the extension's.

    Struct layouts and enum values are only guaranteed inside one major.minor, so a mismatch is a
    wrong library on the path rather than a warning to log.
    """
    lib, hdr = abi_version(), header_abi_version()
    if lib[:2] != hdr[:2]:
        raise BwaError(
            "bw_audio ABI mismatch: the extension was built against {0}.{1}.{2} and the library "
            "reports {3}.{4}.{5}".format(*(hdr + lib))
        )


# ---------------------------------------------------------------------------- device query


def list_devices(backend) -> list:
    """Every device a concrete backend can open, as ``(name, id)`` pairs in index order.

    Needs no engine, so call it before ``Engine(...)`` to fill a device picker. Either string is
    what ``Engine(device=...)`` accepts, matched exactly. AUTO, NULL, MANUAL and a backend this
    build does not carry all report an empty list. Index 0 is the platform default where the
    backend has one.
    """
    out = []
    for i in range(_bwa.get_device_count(backend)):
        out.append((_bwa.get_device_name(backend, i), _bwa.get_device_id(backend, i)))
    return out


# ---------------------------------------------------------------------------- pure solves


def panner_gains(panner, positions, listener, sources, focus: float = 0.0, density: float = 0.0):
    """The per-speaker gains a panner produces, as an ``(nsrc, nspeakers)`` array.

    Pure and reentrant: no engine, no engine state, safe from any thread and alongside a running
    engine. ``focus`` and ``density`` are SPCAP's knobs and are inert under DBAP and VBAP; pass 0
    for the values this array's geometry implies.
    """
    np = _np()
    pos = np.ascontiguousarray(positions, dtype=np.float32).reshape(-1, 3)
    src = np.ascontiguousarray(sources, dtype=np.float32).reshape(-1, 3)
    lis = tuple(float(v) for v in listener)
    return _bwa.panner_gains_batch(panner, pos, lis, src, float(focus), float(density))


def bed_gains(decoder, max_re: bool, positions, directions):
    """The per-speaker gains the diffuse-bed decode produces, as an ``(ndir, nspeakers)`` array.

    Pure and reentrant, like ``panner_gains``. Gains may be negative: those are SH sidelobes.
    """
    np = _np()
    pos = np.ascontiguousarray(positions, dtype=np.float32).reshape(-1, 3)
    dirs = np.ascontiguousarray(directions, dtype=np.float32).reshape(-1, 3)
    return _bwa.bed_gains_batch(decoder, bool(max_re), pos, dirs)


# ---------------------------------------------------------------------------- handles


class _Handle:
    """Common base: a handle value plus the engine that owns it."""

    __slots__ = ("_e", "_h")

    def __init__(self, engine: "Engine", handle: int):
        self._e = engine
        self._h = handle

    @property
    def handle(self) -> int:
        """The raw ``bwa_*`` handle, for calls you make through ``bw_audio._bwa`` yourself."""
        return self._h

    def __int__(self) -> int:
        return self._h

    def __repr__(self) -> str:
        return "<{0} handle=0x{1:08x}>".format(type(self).__name__, self._h)


class Sound(_Handle):
    """A loaded asset.

    ``unload()`` matches ``Engine.load_sound``; ``release()`` matches ``Engine.acquire``. Mixing
    them is refused by the engine, deliberately: freeing an acquired asset would pull it out from
    under the other holders.
    """

    __slots__ = ("_shared",)

    def __init__(self, engine: "Engine", handle: int, shared: bool = False):
        super().__init__(engine, handle)
        self._shared = shared

    @property
    def frames(self) -> int:
        """Length in engine-rate frames. 0 for an invalid handle or a stream of unknown length."""
        self._e._check_thread()
        return _bwa.sound_get_frames(self._e._raw, self._h)

    @property
    def seconds(self) -> float:
        """Length in seconds, derived from the engine's resolved sample rate."""
        return self.frames / float(self._e.sample_rate)

    @property
    def channels(self) -> int:
        """1 for a mono point-source asset, 4/9/16 for an ambisonic bed, 0 for an invalid handle."""
        self._e._check_thread()
        return _bwa.sound_get_channels(self._e._raw, self._h)

    @property
    def ready(self) -> bool:
        """Has an async acquire landed? True for anything acquired synchronously."""
        self._e._check_thread()
        return _bwa.sound_is_ready(self._e._raw, self._h)

    def unload(self) -> None:
        """Unload an asset you LOADED. Safe while it plays (the retire handshake is internal)."""
        self._e._check_thread()
        _bwa.unload_sound(self._e._raw, self._h)

    def release(self) -> None:
        """Drop one reference to an asset you ACQUIRED."""
        self._e._check_thread()
        _bwa.sound_release(self._e._raw, self._h)


class Source(_Handle):
    """One point source. It drives at most one voice: play on a playing source restarts it."""

    # ---- transport
    def play(self, sound, loop: bool = False) -> None:
        self._e._check_thread()
        _bwa.source_play(self._e._raw, self._h, int(sound), loop)

    def play_at(self, sound, start_frame: int, loop: bool = False) -> None:
        """Begin output exactly when the dsp clock reaches ``start_frame``.

        Get "now" from ``Engine.dsp_time_frames`` and add a delay. A start already in the past
        plays immediately; 0 means play now.
        """
        self._e._check_thread()
        _bwa.source_play_at(self._e._raw, self._h, int(sound), loop, int(start_frame))

    def play_loop(self, sound, loop_beg: int = 0, loop_end: int = 0) -> None:
        """Play from the start but wrap from ``loop_end`` back to ``loop_beg`` (intro then body).

        Frames are engine-rate. ``loop_end`` 0 means the clip end.
        """
        self._e._check_thread()
        _bwa.source_play_loop(self._e._raw, self._h, int(sound), int(loop_beg), int(loop_end))

    def queue(self, sound, loop: bool = False) -> None:
        """Chain a sound to start the instant the current one ends. Queue AFTER the play."""
        self._e._check_thread()
        _bwa.source_queue(self._e._raw, self._h, int(sound), loop)

    def clear_queue(self) -> None:
        self._e._check_thread()
        _bwa.source_clear_queue(self._e._raw, self._h)

    def stop(self) -> None:
        """Click-free stop: the voice fades to silence over one block, then ends."""
        self._e._check_thread()
        _bwa.source_stop(self._e._raw, self._h)

    def stop_at(self, stop_frame: int) -> None:
        self._e._check_thread()
        _bwa.source_stop_at(self._e._raw, self._h, int(stop_frame))

    def seek_frames(self, frame: int) -> None:
        """Jump the CONTENT position, in engine-rate frames. In-memory and bed sounds only."""
        self._e._check_thread()
        _bwa.source_seek(self._e._raw, self._h, int(frame))

    def seek_seconds(self, seconds: float) -> None:
        self.seek_frames(int(round(seconds * self._e.sample_rate)))

    def set_region_frames(self, start_frame: int, end_frame: int = 0) -> None:
        """Bound the voice's content to ``[start_frame, end_frame)``. 0 end means the asset end."""
        self._e._check_thread()
        _bwa.source_set_region(self._e._raw, self._h, int(start_frame), int(end_frame))

    def set_region_seconds(self, start_s: float, end_s: float = 0.0) -> None:
        sr = self._e.sample_rate
        self.set_region_frames(int(round(start_s * sr)), int(round(end_s * sr)))

    def fade_to(self, gain: float, seconds: float) -> None:
        self._e._check_thread()
        _bwa.source_fade_to(self._e._raw, self._h, float(gain), float(seconds))

    def fade_out(self, seconds: float) -> None:
        """Fade to silence and then stop: the click-free stop plus cleanup, in one call."""
        self._e._check_thread()
        _bwa.source_fade_out(self._e._raw, self._h, float(seconds))

    def destroy(self) -> None:
        self._e._check_thread()
        _bwa.source_destroy(self._e._raw, self._h)

    # ---- per-frame state
    def set_pos(self, x: float, y: float, z: float) -> None:
        """Room space, meters. Commit-gated: it lands at the next ``Engine.commit()``."""
        self._e._check_thread()
        _bwa.source_set_pos(self._e._raw, self._h, float(x), float(y), float(z))

    def set_orientation(self, qx: float, qy: float, qz: float, qw: float) -> None:
        self._e._check_thread()
        _bwa.source_set_orientation(self._e._raw, self._h, float(qx), float(qy), float(qz), float(qw))

    def set_gain(self, linear: float) -> None:
        self._e._check_thread()
        _bwa.source_set_gain(self._e._raw, self._h, float(linear))

    def set_pitch(self, rate: float) -> None:
        """Playback rate; 1 is native, clamped to [0.25, 4]. In-memory sounds only."""
        self._e._check_thread()
        _bwa.source_set_pitch(self._e._raw, self._h, float(rate))

    def set_paused(self, paused: bool) -> None:
        self._e._check_thread()
        _bwa.source_set_paused(self._e._raw, self._h, bool(paused))

    def set_priority(self, priority: int) -> None:
        """Voice-steal priority, 0 expendable to 255 protected. Default 128."""
        self._e._check_thread()
        _bwa.source_set_priority(self._e._raw, self._h, int(priority))

    def set_group(self, group: int) -> None:
        self._e._check_thread()
        _bwa.source_set_group(self._e._raw, self._h, int(group))

    def set_spread(self, amount: float) -> None:
        """Angular width, 0 a point to 1 wide."""
        self._e._check_thread()
        _bwa.source_set_spread(self._e._raw, self._h, float(amount))

    def set_extent(self, width: float, height: float) -> None:
        self._e._check_thread()
        _bwa.source_set_extent(self._e._raw, self._h, float(width), float(height))

    def set_size(self, radius_m: float) -> None:
        """Physical radius in meters: the rendered width follows the angle it subtends."""
        self._e._check_thread()
        _bwa.source_set_size(self._e._raw, self._h, float(radius_m))

    def set_channel(self, channel: int) -> None:
        """Route this source out of ONE output channel, dry. ``CHANNEL_AUTO`` restores panning."""
        self._e._check_thread()
        _bwa.source_set_channel(self._e._raw, self._h, int(channel))

    def apply(self, desc) -> bool:
        """Push every field of a ``source_desc`` at once. Start from ``source_preset(kind)``."""
        self._e._check_thread()
        return _bwa.source_apply(self._e._raw, self._h, desc)

    def get_desc(self):
        """What this source is CONFIGURED at, or None for a stale handle."""
        self._e._check_thread()
        return _bwa.source_get_desc(self._e._raw, self._h)

    # ---- readbacks
    @property
    def is_playing(self) -> bool:
        """Best-effort poll. Prefer ``Engine.poll_ended()`` for completion."""
        self._e._check_thread()
        return _bwa.source_is_playing(self._e._raw, self._h)

    @property
    def playhead_frames(self) -> int:
        """The CONTENT playhead as of the last rendered block, in engine-rate frames."""
        self._e._check_thread()
        return _bwa.source_get_playhead_frames(self._e._raw, self._h)

    @property
    def playhead_seconds(self) -> float:
        return self.playhead_frames / float(self._e.sample_rate)

    @property
    def occlusion(self) -> float:
        """The sim's live occlusion factor, 1 clear to 0 blocked. A stale handle reads 1."""
        self._e._check_thread()
        return _bwa.source_get_occlusion(self._e._raw, self._h)

    @property
    def directivity(self) -> float:
        """The live directivity gain, 1 on-axis or omni to 0 in a full null."""
        self._e._check_thread()
        return _bwa.source_get_directivity(self._e._raw, self._h)


class PushSource(Source):
    """A source whose voice plays mono float PCM you push, at the engine rate.

    A normal source otherwise: position, gain, spread, groups and fades all apply, while the
    engine rejects play, seek and pitch. Underrun renders silence without losing your place.
    """

    def push(self, frames) -> int:
        """Push mono float32 samples. Returns the count accepted; fewer means the ring is full."""
        self._e._check_thread()
        np = _np()
        buf = np.ascontiguousarray(frames, dtype=np.float32).reshape(-1)
        return _bwa.source_push(self._e._raw, self._h, buf)

    @property
    def space(self) -> int:
        """Frames the ring can take right now. Pace your pushes with it."""
        self._e._check_thread()
        return _bwa.source_push_space(self._e._raw, self._h)

    def push_end(self) -> None:
        """End the voice once the ring drains. One-way: the source is not restartable."""
        self._e._check_thread()
        _bwa.source_push_end(self._e._raw, self._h)


class Bed(_Handle):
    """An ambisonic bed: a world-locked soundfield decoded straight to the speakers.

    It carries no position, and occlusion and directivity do not apply to it.
    """

    def play(self, sound, loop: bool = False) -> None:
        self._e._check_thread()
        _bwa.bed_play(self._e._raw, self._h, int(sound), loop)

    def play_at(self, sound, start_frame: int, loop: bool = False) -> None:
        self._e._check_thread()
        _bwa.bed_play_at(self._e._raw, self._h, int(sound), loop, int(start_frame))

    def play_loop(self, sound, loop_beg: int = 0, loop_end: int = 0) -> None:
        self._e._check_thread()
        _bwa.bed_play_loop(self._e._raw, self._h, int(sound), int(loop_beg), int(loop_end))

    def stop(self) -> None:
        self._e._check_thread()
        _bwa.bed_stop(self._e._raw, self._h)

    def stop_at(self, stop_frame: int) -> None:
        self._e._check_thread()
        _bwa.bed_stop_at(self._e._raw, self._h, int(stop_frame))

    def fade_to(self, gain: float, seconds: float) -> None:
        self._e._check_thread()
        _bwa.bed_fade_to(self._e._raw, self._h, float(gain), float(seconds))

    def fade_out(self, seconds: float) -> None:
        self._e._check_thread()
        _bwa.bed_fade_out(self._e._raw, self._h, float(seconds))

    def set_gain(self, linear: float) -> None:
        self._e._check_thread()
        _bwa.bed_set_gain(self._e._raw, self._h, float(linear))

    def set_orientation(self, yaw_rad: float, pitch_rad: float = 0.0, roll_rad: float = 0.0) -> None:
        """Turn the whole soundfield. Applied roll, then pitch, then yaw."""
        self._e._check_thread()
        _bwa.bed_set_orientation(self._e._raw, self._h, float(yaw_rad), float(pitch_rad), float(roll_rad))

    def set_paused(self, paused: bool) -> None:
        self._e._check_thread()
        _bwa.bed_set_paused(self._e._raw, self._h, bool(paused))

    def seek_frames(self, frame: int) -> None:
        self._e._check_thread()
        _bwa.bed_seek(self._e._raw, self._h, int(frame))

    def seek_seconds(self, seconds: float) -> None:
        self.seek_frames(int(round(seconds * self._e.sample_rate)))

    def set_region_frames(self, start_frame: int, end_frame: int = 0) -> None:
        self._e._check_thread()
        _bwa.bed_set_region(self._e._raw, self._h, int(start_frame), int(end_frame))

    def set_region_seconds(self, start_s: float, end_s: float = 0.0) -> None:
        sr = self._e.sample_rate
        self.set_region_frames(int(round(start_s * sr)), int(round(end_s * sr)))

    def set_priority(self, priority: int) -> None:
        self._e._check_thread()
        _bwa.bed_set_priority(self._e._raw, self._h, int(priority))

    def set_group(self, group: int) -> None:
        self._e._check_thread()
        _bwa.bed_set_group(self._e._raw, self._h, int(group))

    def destroy(self) -> None:
        self._e._check_thread()
        _bwa.bed_destroy(self._e._raw, self._h)

    @property
    def is_playing(self) -> bool:
        self._e._check_thread()
        return _bwa.bed_is_playing(self._e._raw, self._h)

    @property
    def playhead_frames(self) -> int:
        self._e._check_thread()
        return _bwa.bed_get_playhead_frames(self._e._raw, self._h)


class Listener:
    """The listener pose. Skip it entirely when a tracker is connected: the tracker overrides it."""

    __slots__ = ("_e",)

    def __init__(self, engine: "Engine"):
        self._e = engine

    def set_pose(
        self,
        px: float,
        py: float,
        pz: float,
        qx: float = 0.0,
        qy: float = 0.0,
        qz: float = 0.0,
        qw: float = 1.0,
    ) -> None:
        """Room space, meters, plus a head-orientation quaternion. Commit-gated.

        The array render ignores orientation (real speakers, real ears); only the headphone
        renders read it. Identity faces ``ROOM_AHEAD``.
        """
        self._e._check_thread()
        _bwa.set_listener_pose(
            self._e._raw, float(px), float(py), float(pz), float(qx), float(qy), float(qz), float(qw)
        )

    def get_pose(self):
        """The pose the engine is rendering with, as ``((x, y, z), (qx, qy, qz, qw))``."""
        self._e._check_thread()
        return _bwa.get_listener_pose(self._e._raw)

    def set_extra(self, xyz=None) -> None:
        """The OTHER occupants' positions, for compromise panning. None restores one listener."""
        self._e._check_thread()
        if xyz is None:
            _bwa.set_extra_listeners(self._e._raw, None)
            return
        np = _np()
        arr = np.ascontiguousarray(xyz, dtype=np.float32).reshape(-1, 3)
        _bwa.set_extra_listeners(self._e._raw, arr)

    def set_pose_prediction(self, lead_s: float) -> None:
        """Extrapolate the TRACKED position by ``lead_s`` seconds. 0 is off. Needs a tracker."""
        self._e._check_thread()
        _bwa.set_pose_prediction(self._e._raw, float(lead_s))


# ---------------------------------------------------------------------------- engine


class Engine:
    """The engine. Use it as a context manager: enter starts it, exit stops and destroys it.

    ::

        with bw_audio.Engine(profile=Profile.CAVE, sink=SinkType.MANUAL) as e:
            snd = e.load_sound("click.wav")
            src = e.create_source()
            src.set_pos(1.0, 1.5, 0.0)
            e.commit()
            src.play(snd)
            block = e.render_block()

    Every field of ``bwa_desc`` is a keyword argument with the ABI's own zero default. Pass
    ``desc=`` a raw ``_bwa.desc`` instead to set them as a struct.
    """

    def __init__(
        self,
        profile=Profile.CAVE,
        layout_path: Optional[str] = None,
        hrtf_path: Optional[str] = None,
        sample_rate: int = 0,
        block_size: int = 0,
        sink=SinkType.AUTO,
        device: Optional[str] = None,
        embree: bool = False,
        enable_pathing: bool = False,
        bed_decoder=BedDecoder.DEFAULT,
        sink_flags: int = 0,
        desc=None,
        thread_guard: bool = True,
    ):
        if desc is None:
            desc = _bwa.desc()
            desc.profile = profile
            desc.layout_path = layout_path
            desc.hrtf_path = hrtf_path
            desc.sample_rate = int(sample_rate)
            desc.block_size = int(block_size)
            desc.sink = sink
            desc.device = device
            desc.embree = bool(embree)
            desc.enable_pathing = bool(enable_pathing)
            desc.bed_decoder = bed_decoder
            desc.sink_flags = int(sink_flags)

        self._owner_thread = threading.get_ident() if thread_guard else None
        self._started = False
        raw = _bwa.create(desc)
        if raw is None:
            raise BwaError("bwa_create failed: {0}".format(_bwa.last_error(None) or "no reason reported"))
        self._raw = raw
        self.listener = Listener(self)

    # ---- the one control thread
    def _check_thread(self) -> None:
        if self._owner_thread is not None and threading.get_ident() != self._owner_thread:
            raise BwaError(
                "bw_audio: all bwa_* calls must come from ONE control thread. This engine was "
                "created on thread {0} and this is thread {1}. The command ring is "
                "single-producer, so a second producer breaks it. Pass thread_guard=False to "
                "opt out once you funnel your own calls through one thread.".format(
                    self._owner_thread, threading.get_ident()
                )
            )

    # ---- lifecycle
    def start(self) -> None:
        """Open the device(s) and start the audio thread. Blocks; releases the GIL."""
        self._check_thread()
        _check(self._raw, _bwa.start(self._raw), "bwa_start")
        self._started = True

    def stop(self) -> None:
        """Stop the audio thread and close the device(s)."""
        self._check_thread()
        if self._started:
            _check(self._raw, _bwa.stop(self._raw), "bwa_stop")
            self._started = False

    def destroy(self) -> None:
        """Destroy the engine. Idempotent; every later call raises."""
        self._check_thread()
        self._started = False
        _bwa.destroy(self._raw)

    def close(self) -> None:
        """``stop()`` then ``destroy()``."""
        try:
            self.stop()
        finally:
            self.destroy()

    def __enter__(self) -> "Engine":
        self.start()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    # ---- resolved config and status
    @property
    def raw(self):
        """The ``_bwa.engine`` handle, for raw-layer calls you make yourself."""
        return self._raw

    @property
    def sample_rate(self) -> int:
        """The RESOLVED rate in Hz. Derive seconds from this, not from what you asked for."""
        return _bwa.get_sample_rate(self._raw)

    @property
    def block_size(self) -> int:
        """The render quantum in frames. The device may buffer differently."""
        return _bwa.get_block_size(self._raw)

    @property
    def channel_count(self) -> int:
        """The layout's ACTIVE speaker count, 4 to 26. Size your meter arrays with it."""
        return _bwa.get_channel_count(self._raw)

    @property
    def backend(self) -> str:
        """``'<backend>:<device>'`` for logs and HUDs. Never parse it; use ``sink_type``."""
        return _bwa.get_audio_backend(self._raw)

    @property
    def sink_type(self):
        """The backend actually running. Only a SUCCESSFUL start resolves it."""
        return _bwa.get_sink_type(self._raw)

    @property
    def last_error(self) -> Optional[str]:
        """The most recent failure or degradation, or None when clean."""
        return _bwa.last_error(self._raw)

    @property
    def health(self):
        """The dropout counters, or None when this configuration cannot observe a dropout.

        None is not a clean bill of health. A zero ``xruns`` means "none happened" only when this
        is not None: on the manual sink, or before start, it means "never measured".
        """
        return _bwa.get_health(self._raw)

    @property
    def xruns(self) -> int:
        return _bwa.get_xruns(self._raw)

    @property
    def active_voices(self) -> int:
        return _bwa.get_active_voices(self._raw)

    @property
    def speakers(self):
        """The effective layout as an ``(n, 3)`` float32 array, in channel order."""
        return _bwa.get_speakers(self._raw)

    @property
    def bus_levels(self):
        """Last block's per-channel peak, measured after the limiter. Reads 0 until audio runs."""
        return _bwa.get_bus_levels(self._raw)

    # ---- clock
    @property
    def dsp_time_frames(self) -> int:
        """The most recently rendered block's first sample. Monotonic; 0 before the first block."""
        return _bwa.get_dsp_time_frames(self._raw)

    @property
    def dsp_time_seconds(self) -> float:
        return self.dsp_time_frames / float(self.sample_rate)

    @property
    def clock(self):
        """The ``(dsp_sample, host_time_ns)`` pair stamped inside the last block callback.

        None until a host-stamped block renders. This pair is exact, where pairing
        ``dsp_time_frames`` with your own clock read carries up to a block of slack.
        """
        return _bwa.get_clock(self._raw)

    @property
    def clock_model(self):
        """The device-vs-host clock fit (ppm and its error), or None until it has ~1 s of stamps."""
        return _bwa.get_clock_model(self._raw)

    @property
    def output_latency_frames(self) -> int:
        """The device's render-to-DAC delay in frames. Audio scheduled for T is heard at T + this."""
        return _bwa.get_output_latency_frames(self._raw)

    @property
    def output_latency_seconds(self) -> float:
        return self.output_latency_frames / float(self.sample_rate)

    # ---- assets
    def load_sound(self, path: str, streaming: bool = False) -> Sound:
        """Load a mono point-source asset, in RAM or streamed from disk. You unload it."""
        self._check_thread()
        h = _bwa.load_sound_streaming(self._raw, path) if streaming else _bwa.load_sound(self._raw, path)
        if not h:
            raise BwaError("load_sound({0!r}) failed: {1}".format(path, self.last_error or "no reason reported"))
        return Sound(self, h)

    def load_ambix(self, path: str, fuma: bool = False) -> Sound:
        """Load a 4/9/16-channel soundfield for ``Bed.play``. ``fuma`` reads legacy B-format."""
        self._check_thread()
        h = _bwa.load_fuma(self._raw, path) if fuma else _bwa.load_ambix(self._raw, path)
        if not h:
            raise BwaError("load_ambix({0!r}) failed: {1}".format(path, self.last_error or "no reason reported"))
        return Sound(self, h)

    def acquire(self, path: str, flags: int = 0, asynchronous: bool = False) -> Sound:
        """Shared-ownership load from the by-``(path, flags)`` cache. You release it.

        The same key returns the same handle with the refcount bumped. With ``asynchronous``, the
        handle comes back at once and the decode runs on the engine's loader thread; a play issued
        meanwhile is held control-side until the data lands.
        """
        self._check_thread()
        h = (
            _bwa.sound_acquire_async(self._raw, path, flags)
            if asynchronous
            else _bwa.sound_acquire(self._raw, path, flags)
        )
        if not h:
            raise BwaError("acquire({0!r}) failed: {1}".format(path, self.last_error or "no reason reported"))
        return Sound(self, h, shared=True)

    def find(self, path: str, flags: int = 0) -> Optional[Sound]:
        """The cached handle for ``(path, flags)`` if it is already resident, else None.

        A pure lookup: it never loads, never touches the disk and never takes a reference, so do
        not release against it.
        """
        self._check_thread()
        h = _bwa.sound_find(self._raw, path, flags)
        return Sound(self, h, shared=True) if h else None

    # ---- sources, beds
    def create_source(self, desc=None) -> Source:
        """A point source. Pass a ``source_desc`` to configure it before the voice is allocated."""
        self._check_thread()
        h = _bwa.source_create_desc(self._raw, desc) if desc is not None else _bwa.source_create(self._raw)
        if not h:
            raise BwaError("create_source failed: {0}".format(self.last_error or "no reason reported"))
        return Source(self, h)

    def create_push_source(self) -> PushSource:
        """A source you feed with ``PushSource.push``. No file, no play call."""
        self._check_thread()
        h = _bwa.source_create_push(self._raw)
        if not h:
            raise BwaError("create_push_source failed: {0}".format(self.last_error or "no reason reported"))
        return PushSource(self, h)

    def create_bed(self) -> Bed:
        self._check_thread()
        h = _bwa.bed_create(self._raw)
        if not h:
            raise BwaError("create_bed failed: {0}".format(self.last_error or "no reason reported"))
        return Bed(self, h)

    def play_oneshot(self, sound, x: float, y: float, z: float, gain: float = 1.0) -> bool:
        """Fire and forget at a position. No handle; it never steals a voice, so it may be dropped."""
        self._check_thread()
        return _bwa.play_oneshot(self._raw, int(sound), float(x), float(y), float(z), float(gain))

    # ---- events and frame boundary
    def commit(self) -> None:
        """Promote this frame's positions and pose as one snapshot, and drain the event ring.

        Once per frame, last. Forget it and everything renders at its last committed position:
        sounds play, nothing moves.
        """
        self._check_thread()
        _bwa.commit(self._raw)

    def poll_ended(self, cap: int = 64):
        """Handles whose voices ENDED since the last call, and the running dropped total.

        Completion as an event, not a poll: it cannot miss a sound shorter than your frame
        interval, which edge-detecting ``is_playing`` does. Poll it after ``commit()``.
        """
        self._check_thread()
        return _bwa.poll_ended(self._raw, cap)

    def poll_looped(self, cap: int = 64):
        """Handles whose voices WRAPPED at a loop point, and the running dropped total.

        One entry per wrap, not per block: a short loop that wraps several times inside one audio
        block reports every wrap.
        """
        self._check_thread()
        return _bwa.poll_looped(self._raw, cap)

    # ---- mix
    def set_master_gain(self, linear: float) -> None:
        self._check_thread()
        _bwa.set_master_gain(self._raw, float(linear))

    def set_paused(self, paused: bool) -> None:
        self._check_thread()
        _bwa.set_paused(self._raw, bool(paused))

    def stop_all(self) -> None:
        """Stop every voice, click-free, and drop every pending chain. The scene transition."""
        self._check_thread()
        _bwa.stop_all(self._raw)

    def group_set_gain(self, group: int, linear: float) -> None:
        self._check_thread()
        _bwa.group_set_gain(self._raw, int(group), float(linear))

    def group_set_paused(self, group: int, paused: bool) -> None:
        self._check_thread()
        _bwa.group_set_paused(self._raw, int(group), bool(paused))

    def group_stop(self, group: int) -> None:
        self._check_thread()
        _bwa.group_stop(self._raw, int(group))

    # ---- tuning
    def apply_tuning(self, tuning) -> bool:
        """Push a whole ``tuning`` struct. Start from ``tuning_preset(setup)``."""
        self._check_thread()
        return _bwa.apply_tuning(self._raw, tuning)

    def get_tuning(self):
        """Read back what the setters and ``apply_tuning`` have left the engine at."""
        return _bwa.get_tuning(self._raw)

    def set_panner(self, panner) -> None:
        self._check_thread()
        _bwa.set_panner(self._raw, panner)

    def set_limiter(self, on: bool) -> None:
        self._check_thread()
        _bwa.set_limiter(self._raw, bool(on))

    def set_test_signal(self, channel: int, kind, gain: float) -> None:
        """Drive one OUTPUT channel with a built-in tone, injected AFTER the align stage.

        A wiring check, deliberately deaf to the calibration. To hear the trims, route a real
        source with ``Source.set_channel``.
        """
        self._check_thread()
        _bwa.set_test_signal(self._raw, int(channel), kind, float(gain))

    # ---- offline render
    def render_block(self, copy: bool = False):
        """Render exactly one block on THIS thread and return it as ``(channels, nframes)``.

        Manual sink only (``sink=SinkType.MANUAL``). The array is a READ-ONLY view of the
        engine's own planar buffer, valid only until the next ``render_block()`` or ``stop()``,
        so pass ``copy=True`` (or call ``.copy()`` yourself) to keep it. Returns None when the
        engine is not started or the sink is not MANUAL.

        The timestamp is a pure sample counter with no wall clock, so a fixed input and a fixed
        call sequence render bit-identically every run. The async Steam Audio sims are wall-clock
        timed and are NOT reproducible: keep golden renders on the synchronous DSP.
        """
        self._check_thread()
        block = _bwa.render_block(self._raw)
        if block is None:
            return None
        return block.copy() if copy else block

    # ---- tracking
    def tracker_connect(self, **kwargs) -> None:
        """Connect to a NatNet (Motive) stream; the engine then samples the head pose itself.

        Keywords mirror ``bwa_tracker_desc``: ``multicast``, ``server``, ``local_iface``,
        ``data_port``, ``command_port``, ``rigid_body_id``, ``rigid_body_name``,
        ``version_major``, ``version_minor``.
        """
        self._check_thread()
        d = _bwa.tracker_desc()
        for k, v in kwargs.items():
            if not hasattr(d, k):
                raise TypeError("tracker_connect: unknown field {0!r}".format(k))
            setattr(d, k, v)
        _check(self._raw, _bwa.tracker_connect(self._raw, d), "bwa_tracker_connect")

    def tracker_disconnect(self) -> None:
        self._check_thread()
        _bwa.tracker_disconnect(self._raw)

    @property
    def tracker_status(self):
        """What the wire is doing right now. A successful connect only means the socket opened."""
        return _bwa.tracker_status(self._raw)

    # ---- headphone EQ
    def load_headphone_eq(self, path: Optional[str]) -> None:
        """Parse an AutoEq ``ParametricEQ.txt`` into the correction cascade. None clears it."""
        self._check_thread()
        _check(self._raw, _bwa.load_headphone_eq(self._raw, path), "bwa_load_headphone_eq")

    def set_headphone_eq(self, on: bool) -> None:
        self._check_thread()
        _bwa.set_headphone_eq(self._raw, bool(on))

    def __repr__(self) -> str:
        if not self._raw.valid:
            return "<bw_audio.Engine destroyed>"
        return "<bw_audio.Engine {0} {1} Hz / {2} frames / {3} ch>".format(
            self.backend, self.sample_rate, self.block_size, self.channel_count
        )
