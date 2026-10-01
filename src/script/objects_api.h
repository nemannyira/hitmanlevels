#pragma once

namespace h2::script {

// h2.find / h2.findall / h2.objects and the object methods (see objects_api.cpp).
void RegisterObjectsApi();

// Objects created by scripts (h2.spawn, obj:spawn): destroyed before the
// level scripts run again; forgotten when the scene changes (the editor
// destroys its objects when a scene is left).
void DestroyScriptObjects();
void ForgetScriptObjects();

} // namespace h2::script
