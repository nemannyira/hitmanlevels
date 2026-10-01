#pragma once
#include <vector>
// Visual aids drawn over the game view (ImGui background list), after the
// overlays of 3D editors such as Blender: the object's bounding box, a grid
// in the plane it stands on that fades out with distance, its footprint on
// that plane with drop lines, axis lines and the snap target circle.
#include "imgui.h"

#include "model.h"

namespace h2::editor {

// Corners of an object's bounding box (node bounds) at transform `t`;
// corner i: bit 0 = +right, bit 1 = +up, bit 2 = +forward.
struct Box {
    float c[8][3];
};
bool ObjectBox(engine::ZGEOM* geom, const Transform& t, Box& box);
// Box around several parts given in the frame `t` (offsets[i]: part i in that frame).
bool PartsBox(const std::vector<engine::ZGEOM*>& parts, const std::vector<Transform>& offsets, const Transform& t,
              Box& box);
// Centre of the box face that looks most along -normal (its "bottom").
void BoxBottom(const Box& box, const float normal[3], float out[3]);

// Grid spacing for an object of the given size (10 cm ... 10 m).
float GridStep(float size);

// 3D line, clipped at the camera's near plane.
void DrawLine3D(const float a[3], const float b[3], ImU32 color, float thickness = 1.0f);
void DrawBox(const Box& box, ImU32 color, float thickness = 1.5f);

// Grid in the plane through `center` spanned by the unit vectors u and v,
// lines `step` apart on the absolute grid of that plane, fading out towards
// `radius`; every 5th line stronger. With `axes`, the u and v lines through
// `center` are drawn in the axis colours (u red, v blue).
void DrawGrid(const float center[3], const float u[3], const float v[3], float step, float radius, ImU32 color,
              bool axes);

// The box's bottom face projected onto the plane (point, normal): filled, with
// lines from the corners down to it.
void DrawFootprint(const Box& box, const float point[3], const float normal[3], ImU32 fill, ImU32 outline);

// Blender's snap target: a ring with a dot.
void DrawSnapTarget(const float p[3], ImU32 color);

// Screen position of a world point (false when it is behind the camera).
bool WorldToScreen(const float p[3], ImVec2& out);
// Segment with an arrow head at b.
void DrawArrow3D(const float a[3], const float b[3], ImU32 color, float thickness = 2.0f);
// Filled dot (screen radius) with a dark rim.
void DrawPoint3D(const float p[3], float radius, ImU32 color);
// Text next to a world point, on a dark backing.
void DrawLabel3D(const float p[3], const char* text, ImU32 color);

// Unit vectors spanning the plane with `normal`: world X/Z for floors, the
// horizontal and the in-plane "up" direction for walls.
void PlaneAxes(const float normal[3], float u[3], float v[3]);

constexpr ImU32 kAxisX = IM_COL32(232, 64, 72, 255);
constexpr ImU32 kAxisY = IM_COL32(120, 210, 70, 255);
constexpr ImU32 kAxisZ = IM_COL32(70, 130, 240, 255);

} // namespace h2::editor
