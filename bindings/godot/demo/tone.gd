## Writes a short mono 16-bit WAV to user:// and returns its res-style path.
##
## The demo generates its audio instead of shipping a .wav for two reasons: nothing binary
## lands in the repo, and it exercises the staging pattern an exported build actually needs
## — the core opens files by OS path, so anything living inside the .pck has to be written
## out to user:// first (see the README).
## Deliberately no `class_name`: global class names come from the editor's filesystem scan
## cache, which a fresh `--headless` run has not built, so the tests preload this instead.
extends RefCounted


## First-order AmbiX (ACN/SN3D, 4 channels) encoding a plane wave from a ROOM direction.
##
## The ACN axes are ambisonic ones (X front, Y left, Z up), and room space is +Z forward,
## +Y up, with left at +X (BWA_ROOM_RIGHT is -X). So the mapping is:
##   ACN0 = W    ACN1 = Y(left) = room x    ACN2 = Z(up) = room y    ACN3 = X(front) = room z
static func write_ambix(name: String, dir: Vector3, freq: float, seconds: float,
		rate: int = 48000) -> String:
	var d := dir.normalized()
	var gains: Array[float] = [1.0, d.x, d.y, d.z]
	var frames := int(seconds * rate)
	var f := _open_wav("user://%s.wav" % name, 4, frames, rate)
	if f == null:
		return ""
	for i in frames:
		var t := float(i) / float(rate)
		var s: float = sin(TAU * freq * t) * 0.5
		for c in 4:
			f.store_16(int(clampf(s * gains[c], -1.0, 1.0) * 32767.0) & 0xFFFF)
	f.close()
	return "user://%s.wav" % name


static func _open_wav(path: String, channels: int, frames: int, rate: int) -> FileAccess:
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f == null:
		push_error("tone: cannot write %s" % path)
		return null
	var data_bytes := frames * channels * 2
	f.store_string("RIFF")
	f.store_32(36 + data_bytes)
	f.store_string("WAVE")
	f.store_string("fmt ")
	f.store_32(16)                      # PCM chunk size
	f.store_16(1)                       # format = PCM
	f.store_16(channels)
	f.store_32(rate)
	f.store_32(rate * channels * 2)     # byte rate
	f.store_16(channels * 2)            # block align
	f.store_16(16)                      # bits per sample
	f.store_string("data")
	f.store_32(data_bytes)
	return f


static func write_ping(name: String, freq: float, seconds: float, rate: int = 48000) -> String:
	var path := "user://%s.wav" % name
	var frames := int(seconds * rate)
	var f := _open_wav(path, 1, frames, rate)   # mono: what the core wants for a point source
	if f == null:
		return ""

	# A decaying sine: an obvious transient to localize by ear, and it starts and ends near
	# zero so a looped or re-triggered voice has nothing to click on.
	for i in frames:
		var t := float(i) / float(rate)
		var env: float = exp(-3.0 * t)
		var s: float = sin(TAU * freq * t) * env * 0.7
		f.store_16(int(clampf(s, -1.0, 1.0) * 32767.0) & 0xFFFF)

	f.close()
	return path


## THE CLICK: the stimulus every binding's minimal example plays, spelled out once.
##
## One 250 ms period at 48 kHz, holding a 2 ms Hann-windowed noise burst at -12 dBFS peak
## followed by silence. Looped, that is four clicks a second, and an orbit reads as a
## trajectory rather than as a level pan. A tone would not do: the cues that place a source
## in elevation and front-back live in the SPECTRUM, and a narrowband tone carries almost
## none of them.
##
## The noise comes from a 32-bit LCG written out here rather than from randf(), because every
## binding's copy of this stimulus has to produce the same samples and no two languages'
## generators agree. examples/minimal.c is the reference.
const CLICK_PEAK := 0.251189            # -12 dBFS
const CLICK_PERIOD_SECONDS := 0.25      # four clicks a second
const CLICK_BURST_SECONDS := 0.002


## One period of the stimulus: the burst, then silence.
static func click_period(rate: int = 48000) -> PackedFloat32Array:
	var period := int(round(CLICK_PERIOD_SECONDS * rate))
	var nburst := int(round(CLICK_BURST_SECONDS * rate))

	var burst := PackedFloat64Array()
	burst.resize(nburst)
	var state := 12345
	var peak := 0.0
	for i in nburst:
		state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
		var u := float((state >> 8) & 0xFFFFFF) / 8388608.0 - 1.0   # [-1, 1)
		var w := 0.5 - 0.5 * cos(TAU * float(i) / float(nburst - 1))
		burst[i] = w * u
		peak = maxf(peak, absf(burst[i]))

	var scale: float = CLICK_PEAK / peak if peak > 0.0 else 0.0     # exactly -12 dBFS at the crest
	var out := PackedFloat32Array()
	out.resize(period)
	out.fill(0.0)
	for i in nburst:
		out[i] = burst[i] * scale
	return out


## Writes one period as a mono 16-bit wav under user:// and returns its path.
static func write_click(name: String, rate: int = 48000) -> String:
	var samples := click_period(rate)
	var f := _open_wav("user://%s.wav" % name, 1, samples.size(), rate)
	if f == null:
		return ""
	for s in samples:
		f.store_16(int(clampf(s, -1.0, 1.0) * 32767.0) & 0xFFFF)
	f.close()
	return "user://%s.wav" % name
