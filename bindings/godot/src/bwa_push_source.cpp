#include "bwa_push_source.h"

#include <godot_cpp/core/class_db.hpp>

#include "bwa_engine_node.h"

using namespace godot;

#define ENG (owner->handle())
#define LIVE (owner && src && owner->is_running())

bwa_source BwaPushSource::create_source() { return bwa_source_create_push(ENG); }

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
