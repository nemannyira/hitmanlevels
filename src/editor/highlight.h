#pragma once

namespace h2::engine {
struct ZBaseGeom;
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

} // namespace h2::editor
