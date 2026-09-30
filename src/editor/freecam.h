#pragma once

namespace h2::editor {

// Free-flying editor camera. Drives the game's main camera geom through the
// engine's own transform setter right after ZEngineDataBase::FrameUpdate, so
// the engine's moved-geom processing (room / portal membership) runs before
// the frame is rendered.
//
// Controls (editor open, right mouse button held): mouse look, WASD move,
// Q/E down/up, Shift faster, Ctrl slower, mouse wheel changes speed.
void InstallFreeCam();

bool FreeCamEnabled();
void SetFreeCamEnabled(bool enabled);
void FreeCamPosition(float out[3]);
void SetFreeCamPosition(const float pos[3]);

// Camera panel for the overlay.
bool* CameraPanelVisible();
void DrawCameraPanel();

} // namespace h2::editor
