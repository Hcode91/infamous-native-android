#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "ina_game.h"

using namespace godot;

static void initialize(ModuleInitializationLevel level) {
    if (level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
    GDREGISTER_CLASS(InaGame);
}

static void uninitialize(ModuleInitializationLevel) {}

extern "C" GDExtensionBool GDE_EXPORT ina_godot_init(GDExtensionInterfaceGetProcAddress get_proc_address,
                                                      const GDExtensionClassLibraryPtr library,
                                                      GDExtensionInitialization* init) {
    GDExtensionBinding::InitObject obj(get_proc_address, library, init);
    obj.register_initializer(initialize);
    obj.register_terminator(uninitialize);
    obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return obj.init();
}
