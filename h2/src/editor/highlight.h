#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
void* HighlightDevice();  // the game's IDirect3DDevice8 (null before it is made)
// dev: which nodes draw a mesh, at which positions (RenderD3D's DrawNode)
void TraceMeshDraws(uint32_t mesh);
std::string TracedMeshDraws();
void HighlightBeginFrame();

// Whether RenderD3D queued the node for drawing in the last frames (visible,
// not culled, has a mesh). Used to skip invisible objects when picking.
bool WasDrawnRecently(engine::ZBaseGeom* node);

// GPU picking: during the next frame every recognised draw is also rendered
// into a 1x1 target that sees only the pixel (x, y), with the object id as
// colour; the callback gets the object visible there (or null), the world
// position of that surface point and its normal (null when the depth could
// not be read; the normal also when the neighbourhood is not one surface).
using PickCallback = void (*)(engine::ZGEOM* hit, const float* point, const float* normal);
void RequestPick(float x, float y, PickCallback cb);
// An object the picking looks through (e.g. the model being placed).
void SetPickIgnore(engine::ZGEOM* geom);
void SetPickIgnore(const std::vector<engine::ZGEOM*>& geoms);
// A second object drawn with a highlight wireframe (ARGB colour), e.g. the
// model being placed; null clears it.
void SetHighlightExtra(engine::ZGEOM* geom, unsigned long color);
void SetHighlightExtra(const std::vector<engine::ZGEOM*>& geoms, unsigned long color);

// Model thumbnail: the draw calls of `geom` go into a small texture (the
// model turning in front of its own camera) instead of the screen. The
// object must be drawn by the engine (in view, in a visible room). Several
// objects (parts of one model) are shown together.
void SetThumbnailGeom(engine::ZGEOM* geom);
void SetThumbnailGeoms(const std::vector<engine::ZGEOM*>& geoms);
// The thumbnail framed on this sphere (world) and seen from `tilt` (radians, from above) instead of around
// the parts' bounding spheres (a character's: its body, not the bounds of its whole group); radius 0: none.
// The picture turns from looking along world +Z: an object whose forward is -Z shows its front first.
// Cleared by SetThumbnailGeoms with other objects.
void SetThumbnailFrame(const float center[3], float radius, float tilt);
// A list's picture shown now (a tooltip): the part of the thumbnail it is (texture coordinates), how much
// wider it is drawn (a character's: its width stretched by the screen's shape) and what it shows. Twice a
// second its pixels are kept (the render target copied to memory: CopyRects into an image surface), so the
// last picture shown can be saved after the thumbnail is gone (SaveLastPicturePng).
void ThumbnailShown(float u0, float v0, float u1, float v1, float stretch, const std::string& what);
// What the last kept picture shows ("" none yet), and it written as a PNG; false with why.
std::string LastPictureWhat();
bool SaveLastPicturePng(const std::string& path, std::string* error = nullptr);
// IDirect3DTexture8* of the thumbnail (null until something was drawn in it).
void* ThumbnailTexture();
// Frees the thumbnail texture.
void ReleaseThumbnail();
// A picture of an image file (PNG, JPG, BMP: decoded by Windows' WIC, made at most 1024 wide or tall) as an
// IDirect3DTexture8* for ImGui, kept by its path until ReleaseImages; its size in `w` / `h` and the part of
// the texture it fills in `u` / `v` (the texture is a power of two). Null: not read.
void* ImageTexture(const std::string& path, int* w = nullptr, int* h = nullptr, float* u = nullptr, float* v = nullptr);
void ReleaseImages();
// A picture file (PNG, JPG, BMP) written again as a PNG (at most 1024 a side): what a level's laptop photo is
// (a package holds PNGs only); false with why.
bool SavePictureAsPng(const std::string& source, const std::string& path, std::string* error = nullptr);
// References the highlight's resources (thumbnail, depth ramps) hold on
// `device`, and freeing them all (for the device Release hook).
unsigned long HighlightDeviceRefs(void* device);
void ReleaseHighlightResources();

} // namespace h2::editor
