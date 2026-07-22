#include "register_types.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "bwa_bed_node.h"
#include "bwa_emitter_node.h"
#include "bwa_engine_node.h"
#include "bwa_geometry.h"
#include "bwa_material.h"
#include "bwa_push_source.h"
#include "bwa_source_base.h"
#include "bwa_speaker_view.h"

using namespace godot;

void bwa_initialize_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(BwaEngine);
	/* BwaSource carries the shared spatial surface but mints no voice of its own —
	 * abstract, so it cannot be dropped into a scene as a silent no-op. */
	GDREGISTER_ABSTRACT_CLASS(BwaSource);
	GDREGISTER_CLASS(BwaEmitter);
	GDREGISTER_CLASS(BwaPushSource);
	GDREGISTER_CLASS(BwaBed);

	GDREGISTER_CLASS(BwaMaterial);
	GDREGISTER_CLASS(BwaAcousticGeometry);
	GDREGISTER_CLASS(BwaDynamicGeometry);
	GDREGISTER_CLASS(BwaRoomBox);
	GDREGISTER_CLASS(BwaSpeakerView);
}

void bwa_uninitialize_module(ModuleInitializationLevel p_level) {
	(void)p_level;
}

extern "C" {
GDExtensionBool GDE_EXPORT bw_audio_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address,
		const GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(bwa_initialize_module);
	init_obj.register_terminator(bwa_uninitialize_module);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}
