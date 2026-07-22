/* BwaPushSource — a spatial source you feed PCM instead of a file (procedural audio).
 *
 * The full spatial path applies, which is why this shares BwaSource; what differs is that
 * the core REFUSES play/seek/pitch on a push voice, so those simply do not exist here
 * rather than sitting in the inspector doing nothing.
 *
 * It starts consuming at create: silence until the first push. Falling behind (underrun)
 * renders silence without losing your place — the data-driven clock slips, it never drops.
 * Push mono floats at the engine sample rate, from the same single control thread as every
 * other call, at least a frame's worth ahead.
 */
#pragma once

#include <godot_cpp/variant/packed_float32_array.hpp>

#include "bwa_source_base.h"

namespace godot {

class BwaPushSource : public BwaSource {
	GDCLASS(BwaPushSource, BwaSource)

public:
	BwaPushSource() = default;
	~BwaPushSource() override = default;

	/* Returns the count ACCEPTED, which is less than the array size when the ring is full —
	 * pace against push_space() rather than assuming it all landed. */
	int push(const PackedFloat32Array &frames);
	int push_space() const;
	/* End of data: the voice ends once the ring drains, and further pushes are refused.
	 * One-way — a push source is not restartable, create a new one. */
	void push_end();

protected:
	static void _bind_methods();
	bwa_source create_source() override;
};

} // namespace godot
