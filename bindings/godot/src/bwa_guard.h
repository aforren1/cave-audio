/* bwa_guard.h - the one place a bound call refuses an argument OUT LOUD.
 *
 * Every time-valued argument on the transport surface (play_at, play_loop, stop_at, seek_frames,
 * set_region_frames and their seconds twins) reaches the C ABI as an UNSIGNED quantity. So a
 * negative does not fail: it becomes an enormous positive. -1 frames is 1.8e19, about twelve
 * million years of dsp clock, which reads as "schedule this for never" and produces a voice that
 * simply stays silent with nothing logged. That is the same quiet failure BwaSource::set_channel
 * refuses with a warning, so the rest of the surface refuses it the same way rather than half of it
 * dropping the call in silence.
 *
 * The rule these implement: a bound call that REFUSES an argument says so. Not living in a running
 * engine yet is not a refusal - that is normal, and the value is either kept or simply ignored.
 */
#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

inline bool bwa_guard_nonneg(const Node *node, const char *call, const char *arg, int64_t value) {
	if (value >= 0) {
		return true;
	}
	UtilityFunctions::push_warning(vformat("%s (%s): %s refused - %s is %d, and it cannot be "
										   "negative. Nothing was changed.",
			node->get_class(), node->get_name(), call, arg, value));
	return false;
}

inline bool bwa_guard_nonneg(const Node *node, const char *call, const char *arg, double value) {
	if (value >= 0.0) {
		return true;
	}
	UtilityFunctions::push_warning(vformat("%s (%s): %s refused - %s is %f, and it cannot be "
										   "negative. Nothing was changed.",
			node->get_class(), node->get_name(), call, arg, value));
	return false;
}

} // namespace godot
