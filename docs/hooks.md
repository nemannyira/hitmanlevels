# The game functions the editor hooks

MinHook detours (src/hook.h): a hooked function starts with a jmp (0xE9).
Another tool's hook (Frida's Interceptor, a debugger's int3) must not go on
the same function: check its first byte first, and use the editor's own hook
(its logging) instead. Generated from the source (2026-10-18); the ones found
at runtime (Direct3D, RenderD3D.dll, imports) are listed by name only.

| Address | Hook | Source |
|---|---|---|
| 0040E1B0 | ZSysFile::Size | src/engine/script_files.cpp |
| 0040E2C0 | ZSysFile::Exists | src/engine/script_files.cpp |
| 0040E540 | ZSysFile::Read | src/engine/script_files.cpp |
| 0041AAD0 | ZEngineDataBase::SaveGame | src/script/state.cpp |
| 0041AE90 | ZEngineDataBase::DeleteSaveGame | src/script/state.cpp |
| 0041B1C0 | ZEngineDataBase::LoadGame2 | src/script/state.cpp |
| 0041BD80 | GetTreesSize | src/engine/imports.cpp |
| 0041BE10 | GetTreesData | src/engine/imports.cpp |
| 0041BEB0 | ZEngineDataBase::GetStaticSize | src/engine/scene_data.cpp |
| 0041BF40 | ZEngineDataBase::GetStaticData | src/engine/scene_data.cpp |
| 0041BFF0 | GetPrimsSize | src/engine/imports.cpp |
| 0041C080 | GetPrimsData | src/engine/imports.cpp |
| 0041C1C0 | ZEngineDataBase::GetGeomsData | src/engine/scene_data.cpp |
| 0041C270 | GetTextureSize | src/engine/imports.cpp |
| 0041C300 | GetTextureData | src/engine/imports.cpp |
| 0041C760 | GetAnimsSize | src/engine/imports.cpp |
| 0041C7F0 | GetAnimsData | src/engine/imports.cpp |
| 0041CD80 | ZEngineDataBase::RemapCreatedDataBlock | src/engine/scene_data.cpp |
| 0041CDA0 | ZEngineDataBase::CreateGeoms | src/engine/scene_data.cpp |
| 00428FF0 | LOC find | src/editor/mission.cpp |
| 004297F0 | AddSafePointer | src/engine/imports.cpp |
| 0042F3E0 | ZCAMERA::SetupProjection | src/fov.cpp |
| 0043CBE0 | ZGEOM::SendMessage | src/engine/game_events.cpp |
| 004723F0 | ZITEM::SetOwner | src/engine/npc.cpp |
| 00489900 | ZLIST_ActorCollision init | src/engine/npc.cpp |
| 00504410 | Level::InitMessages | src/editor/mission.cpp |
| 005058B0 | Level::MissionCompleted | src/editor/profile_guard.cpp |
| 0050A750 | ZTRACKLNKHM2::DragStart | src/engine/game_events.cpp |
| 0050A820 | ZTRACKLNKHM2::DragEnd | src/engine/game_events.cpp |
| 00519390 | ZHITMAN2::Damage | src/engine/game_events.cpp |
| 0052CED0 | BriefingMenu::Action | src/editor/mission.cpp |
| 0052D340 | BriefingMenu::SetLocaleFolder | src/editor/mission.cpp |
| 0052E2E0 | BriefingMenu::SetObjective | src/editor/mission.cpp |
| 0052E350 | BriefingMenu::SetObjectiveCompleted | src/editor/mission.cpp |
| 0054BF40 | MainMenu::OnButton | src/editor/launcher.cpp |
| 00551420 | Profile::Save | src/editor/profile_guard.cpp |
| 005B2A30 | AllocRefTab (references made while playing) | src/engine/npc.cpp |
| 005C8860 | AnimSetLoad | src/engine/imports.cpp |
| 005E13A0 | Script::Deliver | src/engine/game_events.cpp |

Found at runtime:

* ControlSceneChange (src/diag.cpp)
* Direct3DCreate8 (src/d3dhook.cpp)
* DrawIndexedPrimitive (src/d3dhook.cpp)
* DrawIndexedPrimitiveUP (src/d3dhook.cpp)
* DrawPrimitive (src/d3dhook.cpp)
* DrawPrimitiveUP (src/d3dhook.cpp)
* GatherDynamic (src/devcmd.cpp)
* GatherNodes (src/devcmd.cpp)
* IDirect3D8::CreateDevice (src/d3dhook.cpp)
* IDirect3DDevice8::DrawIndexedPrimitive (src/editor/highlight.cpp)
* IDirect3DDevice8::DrawPrimitive (src/editor/highlight.cpp)
* IDirect3DDevice8::Present (src/d3dhook.cpp)
* IDirect3DDevice8::Release (src/d3dhook.cpp)
* IDirect3DDevice8::SetTransform (src/d3dhook.cpp)
* IDirect3DDevice8::SetTransform (src/editor/highlight.cpp)
* IDirectInput8::CreateDevice (src/input/dinput.cpp)
* IDirectInputDevice8::GetDeviceData (src/input/dinput.cpp)
* IDirectInputDevice8::GetDeviceState (src/input/dinput.cpp)
* IDirectInputDevice8::SetCooperativeLevel (src/input/dinput.cpp)
* IDirectSound::CreateSoundBuffer (src/devcmd.cpp)
* RenderD3D::CacheFlush (src/editor/highlight.cpp)
* RenderD3D::DrawNode (src/editor/highlight.cpp)
* RenderD3D::GetHeight (src/ui.cpp)
* RenderD3D::GetWidth (src/ui.cpp)
* RenderD3D::RangeGive (src/editor/highlight.cpp)
* RenderD3D::RangeTake (src/editor/highlight.cpp)
* RenderD3D::RenderCamera (src/fov.cpp)
* RenderD3D::SetProjection (src/fov.cpp)
* RenderD3D::SetupResolution (src/display.cpp)
* SetVertexShader (src/d3dhook.cpp)
* SetViewport (src/d3dhook.cpp)
* ZEngineDataBase::FrameUpdate (src/editor/freecam.cpp)
* ZEngineDataBase::MainLoop (src/mainloop.cpp)
* ZEngineDataBase::PushScene (src/scene.cpp)
