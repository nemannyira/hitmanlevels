#pragma once

namespace h2::engine {
struct ZBaseGeom;
struct ZGEOM;
}

namespace h2::editor {

// Selection highlight: the selected object (and its children) is drawn a
// second time as a bright see-through wireframe. The batched renderer gives
// no node for a draw call, so draws are matched by their world matrix against
// the selection's expected matrices (computed once per frame).
void InstallHighlight();

// Called by the D3D hooks: device creation, and once per presented frame.
void SetHighlightDevice(void* device);
void HighlightBeginFrame();

// Whether RenderD3D queued the node for drawing in the last frames (visible,
// not culled, has a mesh). Used to skip invisible objects when picking.
bool WasDrawnRecently(engine::ZBaseGeom* node);

// GPU picking: during the next frame every recognised draw is also rendered
// into a 1x1 target that sees only the pixel (x, y), with the object id as
// colour; the callback gets the object visible there (or null) and the world
// position of that surface point (null when the depth could not be read).
using PickCallback = void (*)(engine::ZGEOM* hit, const float* point);
void RequestPick(float x, float y, PickCallback cb);
// An object the picking looks through (e.g. the model being placed).
void SetPickIgnore(engine::ZGEOM* geom);

// Model thumbnail: the draw calls of `geom` go into a small texture (the
// model turning in front of its own camera) instead of the screen. The
// object must be drawn by the engine (in view, in a visible room).
void SetThumbnailGeom(engine::ZGEOM* geom);
// IDirect3DTexture8* of the thumbnail (null until something was drawn in it).
void* ThumbnailTexture();
// Frees the texture; references it holds on `device` (for the Release hook).
void ReleaseThumbnail();
unsigned long ThumbnailDeviceRefs(void* device);

} // namespace h2::editor
