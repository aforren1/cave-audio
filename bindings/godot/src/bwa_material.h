/* BwaMaterial — an acoustic material as a project resource.
 *
 * Save one as a .tres and drop it on every surface made of that stuff. A material is
 * either a built-in preset or custom 3-band coefficients; the engine token it resolves to
 * is minted lazily and cached, because the core's material table is fixed-capacity and
 * meant to be filled once at load.
 *
 * Preset names are an ENUM, not a string. The core answers an unknown material name with
 * the generic default and a note in bwa_last_error — which is not an error, just a wrong
 * sound. An enum makes that unrepresentable.
 */
#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "bw_audio.h"

namespace godot {

class BwaEngine;

class BwaMaterial : public Resource {
	GDCLASS(BwaMaterial, Resource)

public:
	enum Preset {
		PRESET_CUSTOM = -1,
		PRESET_GENERIC = 0, PRESET_BRICK, PRESET_CONCRETE, PRESET_CERAMIC, PRESET_GRAVEL,
		PRESET_CARPET, PRESET_GLASS, PRESET_PLASTER, PRESET_WOOD, PRESET_METAL, PRESET_ROCK,
	};

	BwaMaterial() = default;
	~BwaMaterial() override = default;

	void set_preset(Preset p);
	Preset get_preset() const { return preset; }
	/* Per-band (low, mid, high), each 0..1. Only consulted when preset is CUSTOM. */
	void set_absorption(const Vector3 &v);
	Vector3 get_absorption() const { return absorption; }
	void set_transmission(const Vector3 &v);
	Vector3 get_transmission() const { return transmission; }
	void set_scattering(float v);
	float get_scattering() const { return scattering; }

	/* Mint (or return the cached) engine token. Re-mints when the engine has been rebuilt,
	 * since tokens belong to the engine instance that issued them. */
	bwa_material token(BwaEngine *engine);
	/* The same thing for GDScript, where a token is just an int. */
	int get_token(BwaEngine *engine) { return (int)token(engine); }

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

private:
	Preset preset = PRESET_GENERIC;
	Vector3 absorption = Vector3(0.1f, 0.1f, 0.1f);
	Vector3 transmission = Vector3(0.1f, 0.05f, 0.03f);
	float scattering = 0.05f;

	bwa_material cached = 0;
	int cached_generation = -1;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::BwaMaterial::Preset);
