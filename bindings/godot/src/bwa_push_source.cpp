#include "bwa_push_source.h"

#include <godot_cpp/core/class_db.hpp>

#include "bwa_engine_node.h"

using namespace godot;

#define ENG (owner->handle())
#define LIVE (owner && src && owner->is_running())

/* The one mint with no desc form: bwa_source_create_desc always makes an ORDINARY source, and
 * a push voice is a different creature. So create first, then apply the authored configuration
 * as its own call - the handle is in hand, and the desc is the same one call either way. */
bwa_source BwaPushSource::create_source() {
	const bwa_source s = bwa_source_create_push(ENG);
	if (s) {
		bwa_source_desc d;
		fill_desc(&d);
		bwa_source_apply(ENG, s, &d);
	}
	return s;
}

int BwaPushSource::push(const PackedFloat32Array &frames) {
	if (!LIVE || frames.is_empty()) {
		return 0;
	}
	return (int)bwa_source_push(ENG, src, frames.ptr(), (uint32_t)frames.size());
}

int BwaPushSource::push_space() const { return LIVE ? (int)bwa_source_push_space(ENG, src) : 0; }

void BwaPushSource::push_end() {
	if (LIVE) {
		bwa_source_push_end(ENG, src);
	}
}

void BwaPushSource::_bind_methods() {
	ClassDB::bind_method(D_METHOD("push", "frames"), &BwaPushSource::push);
	ClassDB::bind_method(D_METHOD("push_space"), &BwaPushSource::push_space);
	ClassDB::bind_method(D_METHOD("push_end"), &BwaPushSource::push_end);
}
