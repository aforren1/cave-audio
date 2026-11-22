#!/usr/bin/env python
"""Synthesize an AmbiX direction-check file, then play or render it through the engine.

Each direction gets its own name spoken from that direction ("left" from the left, "up" from
overhead), followed by a short pink-noise burst from the same place. Listen once and you know
whether the bed's heading, its left/right sign and its up/down sign are right. That is a coarse
check: it catches a mirrored or rotated convention, not a few degrees of error.

What it writes:

  directions_ambix.wav       the soundfield: AmbiX (ACN order, SN3D), 4, 9 or 16 channels, 16-bit.
                             Load it in any AmbiX tool, or in the engine with load_ambix.
  directions_binaural.wav    the same field rendered offline through the engine's BINAURAL profile
                             (or CAVE_SIM with --profile cave_sim), for headphones.

The words come from the platform's speech synthesizer: System.Speech on Windows (PowerShell),
`say` on macOS, `espeak-ng` or `espeak` on Linux. Pass --words-dir to use your own recordings
(mono wav files named front.wav, left.wav and so on). With --voice tones, or when no synthesizer
is found, each direction plays a count of noise bursts instead (front 1, back 2, ...), and the
console prints the schedule.

Directions are room directions, as the engine defines them: right-handed, +y up, +z ahead, so +x
is the listener's LEFT. AmbiX has x ahead, y left, z up, so the encode maps (room z, room x,
room y) to (ambi x, y, z). examples/ambisonic.c uses the same mapping.

Headphones cannot reliably tell front from back without head tracking or head movement. Expect
left, right, up and down to be clear, and front against back to be the weakest cue. On the array
(--profile cave, --play) front and back are real.

Run it:      uv run examples/ambisonic_directions.py              writes both files
             uv run examples/ambisonic_directions.py --play       also plays it on headphones
             uv run examples/ambisonic_directions.py --yaw-deg 90 turns the field: "front" from the left
Check it:    uv run examples/ambisonic_directions.py --tests
"""

from __future__ import annotations

import argparse
import math
import os
import shutil
import subprocess
import sys
import tempfile
import time
import wave

import numpy as np

import bw_audio as bwa

SR = 48000
BLK = 256
EAR_HEIGHT_M = 1.5        # the default grid's center; --ear-height-m sets the listener's ears
WORD_PEAK = 0.5           # -6 dBFS: one source at a time, and SN3D keeps every channel at or under W
BURST_SECONDS = 0.35
GAP_SECONDS = 0.6         # silence between directions
LEAD_IN_SECONDS = 0.5     # covers the bed's yaw glide (about one turn a second) before the first word

_S = math.sqrt(0.5)
_E = math.sqrt(1.0 / 3.0)

# (name, the spoken text, the room direction). Room +x is LEFT.
DIRECTIONS = [
    ("front", "front", (0.0, 0.0, 1.0)),
    ("back", "back", (0.0, 0.0, -1.0)),
    ("left", "left", (1.0, 0.0, 0.0)),
    ("right", "right", (-1.0, 0.0, 0.0)),
    ("up", "up", (0.0, 1.0, 0.0)),
    ("down", "down", (0.0, -1.0, 0.0)),
    ("front_left", "front left", (_S, 0.0, _S)),
    ("front_right", "front right", (-_S, 0.0, _S)),
    ("back_left", "back left", (_S, 0.0, -_S)),
    ("back_right", "back right", (-_S, 0.0, -_S)),
    ("up_front", "up front", (0.0, _S, _S)),
    ("up_left", "up left", (_S, _S, 0.0)),
]


# ---- the encode ------------------------------------------------------------------------------

def sh_sn3d(room_dir, order=3):
    """Real SN3D spherical harmonics in ACN order, to `order`, for a ROOM direction."""
    rx, ry, rz = room_dir
    n = math.sqrt(rx * rx + ry * ry + rz * rz)
    x, y, z = rz / n, rx / n, ry / n          # room -> ambi: ahead, left, up
    sh = [
        1.0,
        y, z, x,
        1.7320508 * x * y,
        1.7320508 * y * z,
        0.5 * (3.0 * z * z - 1.0),
        1.7320508 * x * z,
        0.8660254 * (x * x - y * y),
        0.7905694 * y * (3.0 * x * x - y * y),
        3.8729833 * x * y * z,
        0.6123724 * y * (5.0 * z * z - 1.0),
        0.5 * z * (5.0 * z * z - 3.0),
        0.6123724 * x * (5.0 * z * z - 1.0),
        1.9364917 * z * (x * x - y * y),
        0.7905694 * x * (x * x - 3.0 * y * y),
    ]
    return np.array(sh[:(order + 1) ** 2], dtype=np.float64)


# ---- the mono material -----------------------------------------------------------------------

def pink_burst(seconds, seed):
    """Hann-windowed pink noise, 100 Hz to 16 kHz.

    Broadband on purpose: elevation and front-back live in the spectrum. The low cut matters:
    unbounded pink puts most of its energy in the first few bins, where no ear cue lives, and a
    level check over the whole burst then reads that instead of the directional bands.
    """
    n = int(round(seconds * SR))
    rng = np.random.default_rng(seed)
    spec = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, 1.0 / SR)
    gain = np.where((f >= 100.0) & (f <= 16000.0), 1.0 / np.sqrt(np.maximum(f, 1.0)), 0.0)
    x = np.fft.irfft(spec * gain, n)
    x *= np.hanning(n)
    return x / np.abs(x).max()


def read_wav_mono(path):
    """Read a PCM wav with the stdlib, fold it to mono float64, and resample it to SR."""
    with wave.open(path, "rb") as w:
        ch, width, rate, nframes = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(nframes)
    if width == 1:
        x = (np.frombuffer(raw, dtype=np.uint8).astype(np.float64) - 128.0) / 128.0
    elif width == 2:
        x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    elif width == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        x = np.where(v >= 1 << 23, v - (1 << 24), v).astype(np.float64) / float(1 << 23)
    elif width == 4:
        x = np.frombuffer(raw, dtype="<i4").astype(np.float64) / float(1 << 31)
    else:
        raise ValueError("{0}: unsupported sample width {1}".format(path, width))
    x = x.reshape(-1, ch).mean(axis=1)
    if rate != SR:
        # Linear interpolation: fine for a spoken word in a coarse check, not for measurement.
        t = np.arange(int(round(x.size * SR / rate))) * (rate / SR)
        x = np.interp(t, np.arange(x.size), x)
    return x


def trim_silence(x, floor=0.01):
    idx = np.flatnonzero(np.abs(x) > floor * np.abs(x).max())
    return x[idx[0]:idx[-1] + 1] if idx.size else x


def _tts_command(text, path):
    """The platform synthesizer's (command line, extra environment), or None when there is none."""
    if sys.platform == "win32":
        ps = shutil.which("powershell") or shutil.which("pwsh")
        if not ps:
            return None
        # The text and path travel in the environment. -Command joins trailing arguments into the
        # script text, so an argument with a space ("front left") arrives as two.
        script = (
            "Add-Type -AssemblyName System.Speech;"
            "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer;"
            "$f = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo("
            "48000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,"
            " [System.Speech.AudioFormat.AudioChannel]::Mono);"
            "$s.SetOutputToWaveFile($env:BWA_TTS_PATH, $f); $s.Speak($env:BWA_TTS_TEXT); $s.Dispose()"
        )
        return [ps, "-NoProfile", "-NonInteractive", "-Command", script], \
            {"BWA_TTS_TEXT": text, "BWA_TTS_PATH": path}
    if sys.platform == "darwin":
        if not shutil.which("say"):
            return None
        return ["say", "-o", path, "--data-format=LEI16@48000", text], {}
    for exe in ("espeak-ng", "espeak"):
        if shutil.which(exe):
            return [exe, "-w", path, text], {}
    return None


def synthesize_words(texts, workdir):
    """Speak each text to a wav. Returns {text: mono array}, or None when no synthesizer works."""
    out = {}
    for i, text in enumerate(texts):
        path = os.path.join(workdir, "word{0}.wav".format(i))
        found = _tts_command(text, path)
        if found is None:
            return None
        cmd, env = found
        try:
            subprocess.run(cmd, check=True, capture_output=True, timeout=60,
                           env=dict(os.environ, **env))
            out[text] = trim_silence(read_wav_mono(path))
        except (OSError, subprocess.SubprocessError, wave.Error, EOFError, ValueError) as exc:
            print("speech synthesis failed ({0}); falling back to tones".format(exc))
            return None
    return out


def burst_code(count):
    """`count` short bursts, 150 ms apart: the tones-mode stand-in for a spoken name."""
    one = pink_burst(0.08, seed=1000 + count)
    gap = np.zeros(int(0.07 * SR))
    return np.concatenate([np.concatenate([one, gap]) for _ in range(count)])


# ---- the file --------------------------------------------------------------------------------

def build_field(directions, voice, order=3, words_dir=None, noise=True, workdir=None):
    """Lay out every direction in turn and encode it.

    Returns (field as (frames, channels) float64, schedule as [(name, start s, end s)], the
    voice that was actually used).
    """
    words = None
    if voice == "files":
        words = {}
        for name, text, _ in directions:
            path = os.path.join(words_dir, name + ".wav")
            words[text] = trim_silence(read_wav_mono(path))
    elif voice in ("auto", "tts"):
        words = synthesize_words([text for _, text, _ in directions], workdir)
        if words is None and voice == "tts":
            raise RuntimeError("no working speech synthesizer on this machine (try --voice tones)")
    used = "tones" if words is None else ("files" if voice == "files" else "speech")

    segments, schedule = [], []
    t = LEAD_IN_SECONDS
    segments.append(np.zeros((int(LEAD_IN_SECONDS * SR), (order + 1) ** 2)))
    for k, (name, text, room_dir) in enumerate(directions):
        if words is not None:
            mono = words[text] / np.abs(words[text]).max()
        else:
            mono = burst_code(k + 1)
        parts = [mono]
        if noise:
            parts += [np.zeros(int(0.15 * SR)), pink_burst(BURST_SECONDS, seed=k)]
        mono = np.concatenate(parts) * WORD_PEAK
        start = t
        t += mono.size / SR
        schedule.append((name, start, t))
        segments.append(np.outer(mono, sh_sn3d(room_dir, order)))
        gap = int(GAP_SECONDS * SR)
        segments.append(np.zeros((gap, (order + 1) ** 2)))
        t += gap / SR
    return np.concatenate(segments, axis=0), schedule, used


def write_wav(path, frames_by_channel):
    """Write (frames, channels) float as interleaved 16-bit PCM."""
    ints = (np.clip(frames_by_channel, -1.0, 1.0) * 32767.0).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(frames_by_channel.shape[1])
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(np.ascontiguousarray(ints).tobytes())


# ---- the engine ------------------------------------------------------------------------------

def render(path, profile, seconds, yaw_rad=0.0, ear_height_m=EAR_HEIGHT_M):
    """Play the AmbiX file on a bed through the manual sink. Returns (channels, frames) float32."""
    e = bwa.Engine(profile=profile, sample_rate=SR, block_size=BLK, sink=bwa.SinkType.MANUAL)
    try:
        snd = e.load_ambix(path)
        bed = e.create_bed()
        e.listener.set_pose(0.0, ear_height_m, 0.0)     # identity faces room ahead (+z)
        e.start()
        bed.set_orientation(yaw_rad)
        bed.play(snd)
        out = []
        for _ in range(int(math.ceil(seconds * SR / BLK))):
            out.append(e.render_block(copy=True))
        return np.concatenate(out, axis=1), e.speakers
    finally:
        e.close()


def play_live(path, profile, seconds, schedule, yaw_rad=0.0, device=None, ear_height_m=EAR_HEIGHT_M):
    with bwa.Engine(profile=profile, sample_rate=SR, block_size=BLK, device=device) as e:
        snd = e.load_ambix(path)
        bed = e.create_bed()
        e.listener.set_pose(0.0, ear_height_m, 0.0)
        e.start()
        if e.sink_type == bwa.SinkType.NULL:
            print("no output device opened: this run is silent")
        print("backend: {0}".format(e.backend))
        bed.set_orientation(yaw_rad)
        bed.play(snd)
        t0 = time.monotonic()
        for name, start, _ in schedule:
            while time.monotonic() - t0 < start:
                time.sleep(0.005)
            print("  {0:>6.2f} s  {1}".format(start, name.replace("_", " ")))
        while time.monotonic() - t0 < seconds:
            time.sleep(0.02)


# ---- the CLI ---------------------------------------------------------------------------------

PROFILES = {"binaural": bwa.Profile.BINAURAL, "cave_sim": bwa.Profile.CAVE_SIM, "cave": bwa.Profile.CAVE}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="directions_ambix.wav", help="the AmbiX file to write")
    ap.add_argument("--order", type=int, choices=[1, 2, 3], default=3)
    ap.add_argument("--voice", choices=["auto", "tts", "tones"], default="auto",
                    help="auto: speech when a synthesizer is found, else tones")
    ap.add_argument("--words-dir", help="use your own mono recordings, <name>.wav per direction")
    ap.add_argument("--only", help="comma list of direction names, for example left,right,up,down")
    ap.add_argument("--no-noise", action="store_true", help="drop the noise burst after each word")
    ap.add_argument("--profile", choices=sorted(PROFILES), default="binaural",
                    help="what renders the file (cave renders every array channel)")
    ap.add_argument("--yaw-deg", type=float, default=0.0,
                    help="turn the bed; positive turns front toward the left")
    ap.add_argument("--ear-height-m", type=float, default=EAR_HEIGHT_M,
                    help="listener ear height in room meters (the listener stands at x = z = 0)")
    ap.add_argument("--no-render", action="store_true", help="write the AmbiX file only")
    ap.add_argument("--play", action="store_true", help="also play it live on the default device")
    ap.add_argument("--device", help="output device name for --play")
    ap.add_argument("--tests", action="store_true", help="self-check mode, writes nothing")
    args = ap.parse_args(argv)

    if args.tests:
        return selftest()

    directions = DIRECTIONS
    if args.only:
        want = [w.strip() for w in args.only.split(",") if w.strip()]
        known = {d[0]: d for d in DIRECTIONS}
        bad = [w for w in want if w not in known]
        if bad:
            ap.error("unknown direction(s) {0}; known: {1}".format(", ".join(bad), ", ".join(known)))
        directions = [known[w] for w in want]

    voice = "files" if args.words_dir else args.voice
    with tempfile.TemporaryDirectory() as tmp:
        field, schedule, used = build_field(directions, voice, args.order, args.words_dir,
                                            noise=not args.no_noise, workdir=tmp)
    write_wav(args.out, field)
    seconds = field.shape[0] / SR
    print("wrote {0}: order {1}, {2} channels, {3:.1f} s, voice: {4}".format(
        args.out, args.order, field.shape[1], seconds, used))
    for k, (name, start, end) in enumerate(schedule):
        code = "  ({0} burst{1})".format(k + 1, "s" if k else "") if used == "tones" else ""
        print("  {0:>6.2f} s  {1}{2}".format(start, name.replace("_", " "), code))

    profile = PROFILES[args.profile]
    yaw = math.radians(args.yaw_deg)
    if not args.no_render:
        audio, _ = render(args.out, profile, seconds, yaw, args.ear_height_m)
        base, _ = os.path.splitext(args.out)
        rendered = "{0}_{1}.wav".format(base.replace("_ambix", ""), args.profile)
        write_wav(rendered, audio.T.astype(np.float64))
        print("rendered {0} channels through {1} -> {2} (peak {3:.3f})".format(
            audio.shape[0], args.profile, rendered, float(np.abs(audio).max())))
    if args.play:
        play_live(args.out, profile, seconds, schedule, yaw, args.device, args.ear_height_m)
    return 0


# ---- self-check ------------------------------------------------------------------------------

def selftest():
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        print("{0:<52} {1}{2}".format(name, "ok" if ok else "FAIL", ("  " + detail) if detail else ""))
        if not ok:
            failures += 1

    # 1. The encode alone. A basic (projection) decode steered over a dense sphere must peak at the
    # encoded direction. This is what catches a swapped or sign-flipped axis in sh_sn3d itself.
    rng = np.random.default_rng(7)
    probes = rng.standard_normal((4000, 3))
    probes /= np.linalg.norm(probes, axis=1, keepdims=True)
    # ACN/SN3D -> N3D is sqrt(2l+1) per degree; a projection decode wants N3D on both sides.
    n3d = np.repeat([math.sqrt(2 * l + 1) for l in range(4)], [2 * l + 1 for l in range(4)])
    Y = np.array([sh_sn3d(p) * n3d for p in probes])
    worst = 0.0
    for name, _, d in DIRECTIONS:
        beam = Y @ (sh_sn3d(d) * n3d)
        p = probes[int(np.argmax(beam))]
        dn = np.asarray(d) / np.linalg.norm(d)
        worst = max(worst, math.degrees(math.acos(min(1.0, float(p @ dn)))))
    check("encode: every steered peak lands on its direction", worst < 6.0,
          "worst {0:.1f} deg".format(worst))
    # The AmbiX first-order channels, by name: Y is left, Z is up, X is ahead.
    check("encode: room left is +Y (ACN 1)", sh_sn3d((1, 0, 0))[1] > 0.99)
    check("encode: room up is +Z (ACN 2)", sh_sn3d((0, 1, 0))[2] > 0.99)
    check("encode: room ahead is +X (ACN 3)", sh_sn3d((0, 0, 1))[3] > 0.99)

    # 2. Through the engine. Tones mode so the test does not depend on a speech synthesizer.
    six = [d for d in DIRECTIONS if d[0] in ("front", "back", "left", "right", "up", "down")]
    field, schedule, _ = build_field(six, "tones", order=3)
    seconds = field.shape[0] / SR
    tmp = tempfile.mkdtemp()
    try:
        path = os.path.join(tmp, "dirs.wav")
        write_wav(path, field)

        def window(audio, name):
            _, start, end = next(s for s in schedule if s[0] == name)
            return audio[:, int(start * SR):int(end * SR)].astype(np.float64)

        def energy(x):
            return np.sum(x * x, axis=1)

        def ild_db(x):
            # Left over right in 1.5-10 kHz, where the head shadow is large. Below that the level
            # difference is a few dB at most, and it would only dilute the measurement.
            X = np.abs(np.fft.rfft(x, axis=1)) ** 2
            f = np.fft.rfftfreq(x.shape[1], 1.0 / SR)
            e = X[:, (f >= 1500.0) & (f < 10000.0)].sum(axis=1)
            return 10.0 * math.log10(e[0] / max(e[1], 1e-20))

        # 2a. The array: the loudest speakers must sit on the encoded side of the listener. The
        # default grid has speakers on every face, so all six directions are testable.
        cave, spk = render(path, bwa.Profile.CAVE, seconds)
        rel = spk.astype(np.float64) - np.array([0.0, EAR_HEIGHT_M, 0.0])
        rel /= np.maximum(np.linalg.norm(rel, axis=1, keepdims=True), 1e-9)
        for name, _, d in six:
            e = energy(window(cave, name))
            centroid = (e[:, None] * rel).sum(axis=0)
            centroid /= np.linalg.norm(centroid)
            dot = float(centroid @ np.asarray(d))
            check("cave: '{0}' energy centroid points {0}".format(name), dot > 0.7,
                  "cos {0:.2f}".format(dot))

        # 2b. Headphones: left and right by interaural level, from the noise bursts (never DC).
        bin_, _ = render(path, bwa.Profile.BINAURAL, seconds)
        check("binaural render is stereo and finite",
              bin_.shape[0] == 2 and bool(np.all(np.isfinite(bin_))), str(bin_.shape))
        # Lateral sources measure 13 to 24 dB here, the median plane under 3 dB, so 6 dB leaves a
        # margin on both sides.
        for name, sign in (("left", 1.0), ("right", -1.0)):
            ild = ild_db(window(bin_, name))
            check("binaural: '{0}' is louder in the {0} ear".format(name), sign * ild > 6.0,
                  "L-R {0:+.1f} dB".format(ild))
        for name in ("front", "back", "up", "down"):
            ild = ild_db(window(bin_, name))
            check("binaural: '{0}' is near the median plane".format(name), abs(ild) < 4.0,
                  "L-R {0:+.1f} dB".format(ild))

        # 2c. The heading knob: a 90-degree yaw turns 'front' to the left.
        turned, _ = render(path, bwa.Profile.BINAURAL, seconds, yaw_rad=math.pi / 2)
        ild = ild_db(window(turned, "front"))
        check("binaural: yaw +90 deg puts 'front' on the left", ild > 6.0, "L-R {0:+.1f} dB".format(ild))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("FAILURES: {0}".format(failures) if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
