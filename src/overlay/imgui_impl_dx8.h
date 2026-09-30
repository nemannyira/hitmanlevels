#pragma once
// Dear ImGui renderer backend for Direct3D 8 (no official one exists).
// D3D8 has no scissor test, so every draw command is clipped by setting the
// viewport to its clip rect with a matching orthographic projection.
#include "imgui.h"

bool ImGui_ImplDX8_Init(void* device);  // IDirect3DDevice8*
void ImGui_ImplDX8_Shutdown();
void ImGui_ImplDX8_NewFrame();
void ImGui_ImplDX8_RenderDrawData(ImDrawData* drawData);
bool ImGui_ImplDX8_HasDeviceObjects();  // font texture created (holds a device reference)
