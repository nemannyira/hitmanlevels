#pragma once

namespace h2 {

// Hor+ field of view. The engine keeps the camera's *horizontal* FOV fixed and
// derives the vertical one from the viewport aspect (Vert-), so widescreen
// crops the image. While RenderD3D renders a camera we widen its horizontal
// FOV so the vertical FOV matches what the game shows at 4:3; the projection
// and the visibility culling done during the render pass both see it.
void InstallFovFix();

// Multiplier the world camera's tan(fov / 2) currently gets (Hor+ and FovScale).
float HorizontalTanScale();

} // namespace h2
