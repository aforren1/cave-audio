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
#include "bwa_client.h"

namespace godot {

class BwaEngine;

class BwaBed : public Node, public BwaEngineClient {
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
	/* Opt-in async load (bwa_sound_acquire_async). A bed is the common case for it: a
	 * soundfield is 4 to 16 channels of long recording, and a mid-session scene change should
	 * not stall the frame on the decode. The bed binds at once and stays silent until the data
	 * lands, then plays from the top. OFF by default, like BwaEmitter's. */
	void set_async_load(bool v) { async_load = v; }
	bool get_async_load() const { return async_load; }
	/* True between an async play and its data landing. */
	bool is_loading() const;
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
	/* Sample-accurate start against the engine's dsp clock: BwaEngine.get_dsp_time_frames() + n.
	 * The bed twin of BwaEmitter::play_at, on bwa_bed_play_at — a separate ABI call because
	 * the asset check runs the other way round (a bed wants the multichannel file the source
	 * form refuses). */
	void play_at(const String &p, int64_t start_sample);
	/* Intro then looping body inside one soundfield file: [0, loop_beg) plays once,
	 * [loop_beg, loop_end) repeats. 0/0 loops the whole clip. */
	void play_loop(const String &p, int64_t loop_beg, int64_t loop_end);
	/* A scheduled stop on the same dsp clock. It fires `finished`, like BwaEmitter::stop_at:
	 * a scheduled stop is an arranged ending, and the caller wants to know when it landed. */
	void stop_at(int64_t stop_sample);
	void stop();
	void fade_to(float target, float seconds);
	void fade_out(float seconds);
	/* Unit in the name; see BwaEmitter::seek_frames for why a bare seek() is a trap. */
	void seek_frames(int64_t frame);
	void seek_seconds(double seconds);
	/* Bound playback to [start, end) of the soundfield; end 0 means the asset end. A looping
	 * bed wraps back to `start` (and emits `looped`); a one-shot ENDS at `end` exactly as it
	 * would at the asset end (and emits `finished`).
	 *
	 * Call it AFTER a play: the bounds resolve against the bound asset, and any play resets the
	 * region. It does not move the playhead, so a region set mid-play takes effect at the next
	 * boundary. An `end` at or below `start` (and not 0) is refused.
	 *
	 * Named for the unit for the same reason seek_frames is. */
	/* Every time-valued argument on this class is refused, with a warning, if it is NEGATIVE. They
	 * all reach the ABI unsigned, so a negative becomes an enormous positive instead of an error:
	 * -1 frames is 1.8e19, which schedules for never and looks exactly like nothing happening.
	 * See bwa_guard.h. */
	void set_region_frames(int64_t start_frame, int64_t end_frame);
	void set_region_seconds(double start_seconds, double end_seconds);
	/* True while a play is outstanding, which INCLUDES the frame or two before the audio thread has
	 * consumed it and the whole window a play spends held on an async decode. Same rule as
	 * BwaEmitter::is_playing; see the definition. */
	bool is_playing() const;
	int64_t get_playhead_frames() const;
	double get_playhead_seconds() const;

	/* Called by BwaEngine on ITS teardown while this bed still lives (see bwa_client.h). */
	void engine_gone() override;

	/* --- the event feed, driven by BwaEngine (see bwa_engine_node.h's bed registry) --------
	 * A bed IS a voice, so bwa_poll_ended and bwa_poll_looped report bed handles like any
	 * other and these mirror BwaEmitter::notify_ended / notify_looped exactly. They are not
	 * overrides: BwaBed is a Node, not a BwaSource, because a bed has no position — so the
	 * engine keeps a second registry for beds rather than pretend one is a source. */
	void notify_ended();
	void notify_looped();
	/* Run by BwaEngine after the commit AND after both drains, for the same reason
	 * BwaSource::post_commit is: the halt fallback below reads bwa_bed_is_playing, and that
	 * read must not race the event describing the same end. */
	void post_commit();
	/* BwaEngine::group_stop / stop_all stopped this bed. Same rule as stop(): an explicit halt
	 * is not an end, so drop the edge rather than let the silence read as one. */
	void on_stopped_externally();
	/* The native handle, for BwaEngine's handle -> node route. 0 when the create failed. */
	bwa_bed native_handle() const { return bed; }

protected:
	static void _bind_methods();

private:
	bool begin(const String &p, bwa_sound *out);

	BwaEngine *owner = nullptr;
	bwa_bed bed = 0;

	String clip;
	Format format = FORMAT_AMBIX;
	bool loop = true;
	bool autoplay = true;
	bool async_load = false;
	bwa_sound playing_snd = 0; /* the handle play_clip bound, for the readiness probe */
	float gain = 1.0f;
	int priority = 128;
	int group = 0;
	bool paused = false;
	Vector3 orientation;
	Vector3 pushed_orientation;

	/* `finished` has the SAME two feeds as BwaEmitter's, and for the same reason: the core
	 * reports only one of the two ways a voice goes quiet. A bed that RAN OUT (a non-loop
	 * soundfield finished, a region's end) posts a completion EVENT the block it happens, which
	 * BwaEngine drains and routes to notify_ended(). An explicit HALT posts nothing at all
	 * (rt.c takes the click-free stop path for stop / stop_at / fade_out / group_stop /
	 * stop_all), so the is-playing EDGE below survives as the narrow fallback covering exactly
	 * stop_at, which is documented to fire `finished`.
	 *
	 * The three states are BwaEmitter's, and they are load-bearing in both directions. PENDING
	 * absorbs the play WINDOW: bwa_bed_play only ENQUEUES and bwa_bed_is_playing is a per-block
	 * republish, so for a frame or two after a play the bed honestly reads "not playing", and a
	 * bare seen-it-playing flag would read that as an end. IDLE absorbs the HALT window, which
	 * is the same problem mirrored: a stop is enqueued too, so the bed keeps reading as playing
	 * for a block after stop() was told about it. A flag cleared by the halt would simply be set
	 * again by the next post_commit and then fall to silence as a spurious `finished`. IDLE does
	 * not re-arm; only a fresh play leaves it. */
	enum State { IDLE, PENDING, PLAYING };
	State state = IDLE;
	int pending_frames = 0;
	/* The core is HOLDING this play: it did not bind, so the bed honestly reads "not playing"
	 * until the decode lands. Both non-binding answers set it - still decoding, and a decode that
	 * already failed - because everything that turns on it (the duplicate-end latch, the failure
	 * report, is_loading) is really asking "has this play bound yet", and a failed one never will.
	 * It stays off on the synchronous path, where the readiness probe would clear bwa_last_error
	 * every frame for an answer that is always true. */
	bool pending_async = false;
	String pending_path; /* what begin() was handed, for the failure message */
	/* A play can be DROPPED (a full command ring) or its voice STOLEN at onset, and neither
	 * posts a completion. Past this many frames the readback is taken at its word and the claim
	 * dropped, silently: not knowing whether a bed ended is not knowing that it did. */
	static constexpr int PENDING_GRACE = 4;
	/* ONE end can still reach both feeds. rt.c's mix seam takes `ended = true` for a voice that
	 * is already `stopping` and whose asset runs out in the same block, so a halt can be
	 * followed by a genuine completion event for the same play. This latch makes whichever feed
	 * reports first the only one that does. Cleared by the next play that BINDS, so a swallowed
	 * straggler can never eat a LATER end - and a play still HELD on an async decode has not
	 * bound, so it does not clear it (see begin(): the core has not bumped play_seq for a held
	 * play, so the straggler is still deliverable and the latch is all that stands in its way).
	 * on_stopped_externally() arms it too: `finished` must not fire for a scene change, and that
	 * interleaving is exactly how it could. */
	bool end_fired = false;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::BwaBed::Format);
