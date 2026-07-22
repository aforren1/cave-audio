## Test-signal synthesis for the playground, ported sample-for-sample from
## examples/playground.cpp's gen_signal / gen_bed.
##
## The port is deliberately literal — same LCG seeds, same Paul Kellet pink filter
## coefficients, same periods. Structurally identical, not bit-identical: GDScript computes
## in float64 where the C pipeline is float32, so low-order bits differ. That is inaudible,
## which is what matters — if the Godot playground and the C++ one are ever A/B'd against
## each other, any audible difference has to come from the ENGINE path, not the noises.
##
## Signal choice matters: broadband content with sharp onsets localises best, and it is the
## HF that carries elevation and front/back. The 1 kHz tone is in the list precisely because
## it is narrowband and ambiguous — it is the control.
extends RefCounted

const SR := 48000
const SIG_SECS := 2
const SIGLEN := SR * SIG_SECS

const NAMES: Array[String] = [
	"pink noise", "pink bursts", "click train", "1 kHz tone (ambiguous)"
]
const FILES: Array[String] = [
	"user://pg_pink.wav", "user://pg_bursts.wav", "user://pg_clicks.wav", "user://pg_tone.wav"
]
const BED_FILE := "user://pg_bed.wav"

# The bed's marker bearings in ROOM space: bursts from the FRONT, clicks from LEFT-UP. The
# playground draws spheres on these, turned by the live bed orientation, so you can see where
# the field claims its content is while you hear where it actually is.
const BED_DIRS: Array[Vector3] = [Vector3(0, 0, 1), Vector3(0.87, 0.5, 0)]

## One noise stream: an LCG plus the pink filter's state. The bed needs three INDEPENDENT
## streams (burst, click, diffuse floor) so its three components stay mutually uncorrelated,
## which is why this is an object rather than module-level state.
class Stream extends RefCounted:
	var lcg: int
	var b: PackedFloat64Array

	func _init(seed_value: int) -> void:
		lcg = seed_value
		b = PackedFloat64Array()
		b.resize(7)

	## 32-bit LCG matching the C++ `white()`, bit-slicing included. GDScript ints are
	## 64-bit, so every step masks back down to 32 or the sequence diverges immediately.
	func white() -> float:
		lcg = (lcg * 1664525 + 1013904223) & 0xFFFFFFFF
		return float((lcg >> 9) - (1 << 22)) / float(1 << 22)

	## Paul Kellet's pink filter, same coefficients as the C++ side.
	func pink() -> float:
		var w := white()
		b[0] = 0.99886 * b[0] + w * 0.0555179
		b[1] = 0.99332 * b[1] + w * 0.0750759
		b[2] = 0.96900 * b[2] + w * 0.1538520
		b[3] = 0.86650 * b[3] + w * 0.3104856
		b[4] = 0.55000 * b[4] + w * 0.5329522
		b[5] = -0.7616 * b[5] - w * 0.0168980
		var p: float = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362
		b[6] = w * 0.115926
		return p * 0.11


## Write every signal and the bed. Returns the mono file paths; the bed path is BED_FILE.
func generate_all() -> Array[String]:
	for i in NAMES.size():
		_write_mono(FILES[i], _gen_signal(i))
	_write_bed()
	return FILES


func _gen_signal(which: int) -> PackedFloat32Array:
	var s := Stream.new(22222)
	var buf := PackedFloat32Array()
	buf.resize(SIGLEN)
	var period := SR / 5   # 200 ms, and it divides SIGLEN exactly so the loop seam is silent

	match which:
		0:  # continuous pink noise
			for i in SIGLEN:
				buf[i] = s.pink() * 0.55
		1:  # pink bursts: 100 ms on / 100 ms off, 5 ms fades
			var on := period / 2
			var ramp := SR / 200
			for i in SIGLEN:
				var p := s.pink() * 0.55
				var ph := i % period
				var env := 0.0
				if ph < ramp:
					env = float(ph) / ramp
				elif ph < on - ramp:
					env = 1.0
				elif ph < on:
					env = float(on - ph) / ramp
				buf[i] = p * env
		2:  # click train: 5/s, ~3 ms decaying broadband ticks
			var clicklen := SR / 333
			for i in SIGLEN:
				var ph := i % period
				buf[i] = s.white() * exp(-6.0 * float(ph) / clicklen) * 0.6 if ph < clicklen else 0.0
		_:  # 1 kHz sine - narrowband, ambiguous on purpose
			for i in SIGLEN:
				buf[i] = sin(TAU * 1000.0 * float(i) / SR) * 0.3
	return buf


## SN3D real spherical harmonics to order 3 for a ROOM direction.
##
## The axis swap is the whole subtlety: ambisonic axes are x=front, y=left, z=up, and room
## space is +z front, +x left, +y up. A local copy of the encode keeps the playground a pure
## ABI client, exactly as the C++ one does.
func _sh16_room(r: Vector3) -> PackedFloat32Array:
	var n := r.normalized()
	var x := n.z
	var yy := n.x
	var z := n.y
	var y := PackedFloat32Array()
	y.resize(16)
	y[0] = 1.0
	y[1] = yy;  y[2] = z;  y[3] = x
	y[4] = 1.7320508 * x * yy
	y[5] = 1.7320508 * yy * z
	y[6] = 0.5 * (3.0 * z * z - 1.0)
	y[7] = 1.7320508 * x * z
	y[8] = 0.8660254 * (x * x - yy * yy)
	y[9] = 0.7905694 * yy * (3.0 * x * x - yy * yy)
	y[10] = 3.8729833 * x * yy * z
	y[11] = 0.6123724 * yy * (5.0 * z * z - 1.0)
	y[12] = 0.5 * z * (5.0 * z * z - 3.0)
	y[13] = 0.6123724 * x * (5.0 * z * z - 1.0)
	y[14] = 1.9364917 * z * (x * x - yy * yy)
	y[15] = 0.7905694 * x * (x * x - 3.0 * yy * yy)
	return y


## A 3rd-order AmbiX field: pink bursts from the front, clicks from left-up in the bursts'
## gaps, and a W-only pink floor that reads as diffuse. Three distinguishable things in three
## places is what makes a rotation audible rather than merely different.
func _write_bed() -> void:
	var yf := _sh16_room(BED_DIRS[0])
	var yl := _sh16_room(BED_DIRS[1])
	var period := SR / 2
	var on := period / 2
	var ramp := SR / 100
	var clicklen := SR / 333

	var buf := PackedFloat32Array()
	buf.resize(SIGLEN * 16)

	# Three independent streams, so the burst, the click and the diffuse floor stay mutually
	# uncorrelated the way the C++ version's three seeds make them.
	var s1 := Stream.new(33333)
	var s2 := Stream.new(44444)
	var s3 := Stream.new(55555)

	for i in SIGLEN:
		var ph := i % period
		var env := 0.0
		if ph < ramp:
			env = float(ph) / ramp
		elif ph < on - ramp:
			env = 1.0
		elif ph < on:
			env = float(on - ph) / ramp
		var burst := s1.pink() * 0.5 * env
		var pc := (i + period / 2) % period          # clicks land in the bursts' gaps
		var click := s2.white() * exp(-6.0 * float(pc) / clicklen) * 0.7 if pc < clicklen else 0.0
		var dif := s3.pink() * 0.08                  # W only
		var base := i * 16
		for k in 16:
			buf[base + k] = burst * yf[k] + click * yl[k]
		buf[base] += dif

	_write_wav(BED_FILE, buf, 16)


func _write_mono(path: String, buf: PackedFloat32Array) -> void:
	_write_wav(path, buf, 1)


## 32-bit float WAV, matching the C++ writer's format so the two playgrounds load byte-alike
## assets rather than one quantised copy.
func _write_wav(path: String, samples: PackedFloat32Array, channels: int) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f == null:
		push_error("signals: cannot write %s" % path)
		return
	var data_bytes := samples.size() * 4
	f.store_string("RIFF")
	f.store_32(36 + data_bytes)
	f.store_string("WAVE")
	f.store_string("fmt ")
	f.store_32(16)
	f.store_16(3)                       # IEEE float
	f.store_16(channels)
	f.store_32(SR)
	f.store_32(SR * channels * 4)
	f.store_16(channels * 4)
	f.store_16(32)
	f.store_string("data")
	f.store_32(data_bytes)
	f.store_buffer(samples.to_byte_array())
	f.close()
