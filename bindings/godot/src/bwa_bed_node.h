/* BwaBed — a world-locked ambisonic soundfield decoded straight to the speakers.
 *
 * A Node, not a Node3D: a bed has no position. It has an ORIENTATION, which is a different
 * thing — it turns the whole field, for lining a capture up with the scene or levelling one
 * that was not captured upright.
 *
 * A bed is a voice like any other (same pool, same steal priority, same groups, same
 * fades), so those controls mirror the source ones. Occlusion, directivity, spread and
 * distance do not apply, and are absent rather than inert.
 */
#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "bw_audio.h"

namespace godot {

class BwaEngine;

class BwaBed : public Node {
	GDCLASS(BwaBed, Node)

public:
	/* AmbiX (ACN/SN3D) is the modern convention; FuMa is legacy B-format (WXYZ, MaxN, W at
	 * -3 dB), converted at load so downstream a FuMa bed IS an AmbiX bed. */
	enum Format { FORMAT_AMBIX = 0, FORMAT_FUMA = 1 };

	BwaBed() = default;
	~BwaBed() override = default;

	void _ready() override;
	void _process(double delta) override;
	void _exit_tree() override;

	void set_clip(const String &p) { clip = p; }
	String get_clip() const { return clip; }
	void set_format(Format f) { format = f; }
	Format get_format() const { return format; }
	void set_loop(bool v) { loop = v; }
	bool get_loop() const { return loop; }
	void set_autoplay(bool v) { autoplay = v; }
	bool get_autoplay() const { return autoplay; }
	void set_gain(float g);
	float get_gain() const { return gain; }
	void set_priority(int p);
	int get_priority() const { return priority; }
	void set_group(int g);
	int get_group() const { return group; }
	void set_paused(bool p);
	bool get_paused() const { return paused; }

	/* Room-frame yaw/pitch/roll in RADIANS. Positive yaw turns the field from room +Z
	 * toward +X; positive pitch tilts its front up; positive roll tilts its top toward
	 * room -X. Glides at about one turn a second, so it is safe to animate. */
	void set_orientation(const Vector3 &ypr);
	Vector3 get_orientation() const { return orientation; }
	/* Yaw for a Godot facing direction, through the coordinate seam — the shorthand that
	 * keeps callers from passing a Godot euler angle straight in. */
	void set_yaw_from_basis(const Basis &b);

	void play();
	void play_clip(const String &p);
	void stop();
	void fade_to(float target, float seconds);
	void fade_out(float seconds);
	void seek(int64_t frame);
	bool is_playing() const;
	int64_t get_playhead() const;
	double get_playhead_seconds() const;

protected:
	static void _bind_methods();

private:
	BwaEngine *owner = nullptr;
	bwa_bed bed = 0;

	String clip;
	Format format = FORMAT_AMBIX;
	bool loop = true;
	bool autoplay = true;
	float gain = 1.0f;
	int priority = 128;
	int group = 0;
	bool paused = false;
	Vector3 orientation;
	Vector3 pushed_orientation;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::BwaBed::Format);
