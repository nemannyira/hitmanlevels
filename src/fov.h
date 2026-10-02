#pragma once

namespace h2::game {
struct ZCAMERA;
}

namespace h2 {

// Hor+ field of view. The engine keeps the camera's *horizontal* FOV fixed and
// derives the vertical one from the viewport aspect (Vert-), so widescreen
// crops the image. World cameras get a horizontal FOV widened so the vertical
// FOV matches what the game shows at 4:3 - in the camera itself, so the
// renderer and the engine's own culling (characters) both use it. Scope,
// binocular and night vision views keep the game's FOV (their frame models
// are made for it).
void InstallFovFix();

// tan(horizontal fov / 2) of what the camera shows (Hor+ included).
float CameraTanHalfFov(const game::ZCAMERA* cam);
// Scene change: camera objects are gone.
void ForgetCameras();

// Debug: log every camera rendered during the next `frames` frames.
void TraceCameras(int frames);
void TraceFrameDone();  // called once per presented frame
bool TracingCameras();

} // namespace h2
