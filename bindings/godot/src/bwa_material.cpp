#include "bwa_material.h"

#include <godot_cpp/core/class_db.hpp>

#include "bwa_engine_node.h"

using namespace godot;

/* Edits set `dirty` rather than clobbering cached_generation: the generation must keep
 * recording which engine instance minted `cached`, or the release below could hand a
 * dead engine's token number to a live engine's table. */
void BwaMaterial::set_preset(Preset p) {
	preset = p;
	dirty = true;
	notify_property_list_changed(); // show/hide the custom coefficients
}

void BwaMaterial::set_absorption(const Vector3 &v) {
	absorption = v;
	dirty = true;
}

void BwaMaterial::set_transmission(const Vector3 &v) {
	transmission = v;
	dirty = true;
}

void BwaMaterial::set_scattering(float v) {
	scattering = v;
	dirty = true;
}

bwa_material BwaMaterial::token(BwaEngine *engine) {
	if (!engine || !engine->is_running()) {
		return 0;
	}
	/* Tokens index a table owned by one engine instance, so a rebuilt engine invalidates
	 * them. Keying the cache on the engine's generation is what stops a stale token from
	 * quietly addressing whatever now lives in that slot. */
	if (!dirty && cached_generation == engine->get_generation()) {
		return cached;
	}
	/* Re-minting after an EDIT on the same live engine: release the superseded token first,
	 * or live-tuning a coefficient bleeds the 64-slot table one slot per edit. Safe per the
	 * ABI (bw_audio.h, bwa_material_release): meshes copy the material at set time, so
	 * geometry already carrying the old token is unaffected, and the core refuses token 0.
	 * A generation MISMATCH means the token belongs to a torn-down engine - nothing to
	 * release, and the number must not reach this engine's table. */
	if (cached != 0 && cached_generation == engine->get_generation()) {
		engine->material_release((int)cached);
	}
	if (preset == PRESET_CUSTOM) {
		cached = (bwa_material)engine->material_define(absorption, scattering, transmission);
	} else {
		cached = (bwa_material)engine->material_preset((BwaEngine::Material)preset);
	}
	cached_generation = engine->get_generation();
	dirty = false;
	return cached;
}

/* The custom coefficients are meaningless for a preset, so hide them rather than let
 * someone tune numbers that will never be read. */
void BwaMaterial::_validate_property(PropertyInfo &p_property) const {
	if (preset != PRESET_CUSTOM &&
			(p_property.name == StringName("absorption") ||
					p_property.name == StringName("transmission") ||
					p_property.name == StringName("scattering"))) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

void BwaMaterial::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_preset", "preset"), &BwaMaterial::set_preset);
	ClassDB::bind_method(D_METHOD("get_preset"), &BwaMaterial::get_preset);
	ClassDB::bind_method(D_METHOD("set_absorption", "bands"), &BwaMaterial::set_absorption);
	ClassDB::bind_method(D_METHOD("get_absorption"), &BwaMaterial::get_absorption);
	ClassDB::bind_method(D_METHOD("set_transmission", "bands"), &BwaMaterial::set_transmission);
	ClassDB::bind_method(D_METHOD("get_transmission"), &BwaMaterial::get_transmission);
	ClassDB::bind_method(D_METHOD("set_scattering", "amount"), &BwaMaterial::set_scattering);
	ClassDB::bind_method(D_METHOD("get_scattering"), &BwaMaterial::get_scattering);
	ClassDB::bind_method(D_METHOD("get_token", "engine"), &BwaMaterial::get_token);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "preset", PROPERTY_HINT_ENUM,
						 "Custom:-1,Generic:0,Brick,Concrete,Ceramic,Gravel,Carpet,Glass,Plaster,"
						 "Wood,Metal,Rock"),
			"set_preset", "get_preset");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "absorption"), "set_absorption", "get_absorption");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "transmission"), "set_transmission",
			"get_transmission");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scattering", PROPERTY_HINT_RANGE, "0,1,0.01"),
			"set_scattering", "get_scattering");

	BIND_ENUM_CONSTANT(PRESET_CUSTOM);
	BIND_ENUM_CONSTANT(PRESET_GENERIC); BIND_ENUM_CONSTANT(PRESET_BRICK);
	BIND_ENUM_CONSTANT(PRESET_CONCRETE); BIND_ENUM_CONSTANT(PRESET_CERAMIC);
	BIND_ENUM_CONSTANT(PRESET_GRAVEL); BIND_ENUM_CONSTANT(PRESET_CARPET);
	BIND_ENUM_CONSTANT(PRESET_GLASS); BIND_ENUM_CONSTANT(PRESET_PLASTER);
	BIND_ENUM_CONSTANT(PRESET_WOOD); BIND_ENUM_CONSTANT(PRESET_METAL);
	BIND_ENUM_CONSTANT(PRESET_ROCK);
}
