#pragma once
#include <cstdint>
#include <string>

#include "../engine/geoms.h"

struct lua_State;

namespace h2::script {

// h2.find / h2.findall / h2.objects and the object methods (see objects_api.cpp).
void RegisterObjectsApi();

// Objects created by scripts (h2.spawn, obj:spawn): destroyed before the
// level scripts run again; forgotten when the scene changes (the editor
// destroys its objects when a scene is left).
void DestroyScriptObjects();
void ForgetScriptObjects();
// Pushes the object handle of a scene path (for events).
void PushObjectPath(::lua_State* L, const std::string& path);
// Pushes the object of a scene object pointer, or nil (false) when it is not
// one of the loaded scene's objects (the pointer is only compared).
bool PushGeom(::lua_State* L, engine::ZGEOM* geom);
// The same for a value an engine message carries: a pointer or a geom ref.
bool PushCarriedGeom(::lua_State* L, uint32_t value);
// The object whose script is `instance` (messages between scripts), or nil.
bool PushScriptGeom(::lua_State* L, void* instance);

} // namespace h2::script
