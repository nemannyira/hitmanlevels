#include "api.h"

#include <cmath>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "imgui.h"

#include "../display.h"
#include "../editor/freecam.h"
#include "../editor/playtest.h"
#include "../editor/testtools.h"
#include "../engine/messages.h"
#include "../log.h"
#include "../scene.h"
#include "../ui.h"
#include "lua_vm.h"

namespace h2::script {

namespace {

// h2.sound(name) -> id | nil: one of the level's sounds by its name, as the
// scripts' Sound_If FindWave + Play2DSound play it; h2.stopsound(id)
int Sound(lua_State* L)
{
    uint32_t ref = engine::PlaySound2D(luaL_checkstring(L, 1));
    if (!ref)
        return 0;
    lua_pushinteger(L, lua_Integer(ref));
    return 1;
}

int StopSoundFn(lua_State* L)
{
    lua_pushboolean(L, engine::StopSound(uint32_t(luaL_checkinteger(L, 1))));
    return 1;
}

// h2.fps() -> number
int Fps(lua_State* L)
{
    lua_pushnumber(L, ImGui::GetCurrentContext() ? ImGui::GetIO().Framerate : 0.0);
    return 1;
}

// h2.scene() -> string: currently loaded scene, e.g. "C1-1\C1-1__MAIN"
int Scene(lua_State* L)
{
    lua_pushstring(L, CurrentScene().c_str());
    return 1;
}

// h2.resolution() -> width, height (back buffer)
int Resolution(lua_State* L)
{
    lua_pushinteger(L, RenderWidth());
    lua_pushinteger(L, RenderHeight());
    return 2;
}

// h2.uiscale() -> real / virtual UI scale
int UiScaleFn(lua_State* L)
{
    lua_pushnumber(L, UiScale());
    return 1;
}

// h2.freecam([enable]) -> enabled
int FreeCam(lua_State* L)
{
    if (lua_gettop(L) >= 1)
        editor::SetFreeCamEnabled(lua_toboolean(L, 1));
    lua_pushboolean(L, editor::FreeCamEnabled());
    return 1;
}

// h2.speed([factor]) -> factor: game speed while a map is loaded (1 normal, 0.1 .. 8)
int Speed(lua_State* L)
{
    if (lua_gettop(L) >= 1)
        editor::SetGameSpeed(float(luaL_checknumber(L, 1)));
    lua_pushnumber(L, editor::GameSpeed());
    return 1;
}

// h2.god([on]) -> on: Hitman cannot be hurt
int God(lua_State* L)
{
    if (lua_gettop(L) >= 1)
        editor::SetGodMode(lua_toboolean(L, 1));
    lua_pushboolean(L, editor::GodMode());
    return 1;
}

// h2.invisible([on]) -> on: no one sees Hitman (characters do not see anyone)
int Invisible(lua_State* L)
{
    if (lua_gettop(L) >= 1)
        editor::SetHitmanInvisible(lua_toboolean(L, 1));
    lua_pushboolean(L, editor::HitmanInvisible());
    return 1;
}

// h2.freeze([on]) -> on: the world stands still (also with the editor closed)
int Freeze(lua_State* L)
{
    if (lua_gettop(L) >= 1)
        editor::SetWorldFrozenWhilePlaying(lua_toboolean(L, 1));
    lua_pushboolean(L, editor::WorldFrozenWhilePlaying());
    return 1;
}

// h2.campos() -> x, y, z (camera position, centimetres, +Y up)
int CamPos(lua_State* L)
{
    float p[3];
    editor::FreeCamPosition(p);
    for (float v : p)
        lua_pushnumber(L, v);
    return 3;
}

// a finite number in the world's range (a NaN camera breaks the renderer's culling)
float Finite(lua_State* L, int arg)
{
    double v = luaL_checknumber(L, arg);
    if (!std::isfinite(v) || std::fabs(v) > 1e7)
        luaL_argerror(L, arg, "not a usable number");
    return float(v);
}

// h2.setcampos(x, y, z): moves the free camera
int SetCamPos(lua_State* L)
{
    float p[3] = {Finite(L, 1), Finite(L, 2), Finite(L, 3)};
    editor::SetFreeCamPosition(p);
    return 0;
}

// h2.setcamrot(yaw, pitch): free camera direction in degrees (yaw 0 = +Z, 90 = +X; pitch > 0 up)
int SetCamRot(lua_State* L)
{
    editor::SetFreeCamAngles(Finite(L, 1), lua_isnoneornil(L, 2) ? 0.0f : Finite(L, 2));
    return 0;
}

// h2.camrot() -> yaw, pitch (degrees)
int CamRot(lua_State* L)
{
    float yaw, pitch;
    editor::FreeCamAngles(yaw, pitch);
    lua_pushnumber(L, yaw);
    lua_pushnumber(L, pitch);
    return 2;
}

// h2.camlookat(x, y, z): turns the free camera towards a point
int CamLookAt(lua_State* L)
{
    float p[3];
    editor::FreeCamPosition(p);
    float d[3] = {Finite(L, 1) - p[0], Finite(L, 2) - p[1], Finite(L, 3) - p[2]};
    float yaw = std::atan2(d[0], d[2]) * 57.29578f;
    float pitch = std::atan2(d[1], std::sqrt(d[0] * d[0] + d[2] * d[2])) * 57.29578f;
    editor::SetFreeCamAngles(yaw, pitch);
    return 0;
}

// h2.log(...) -> writes to H2Fix.log
int LogFn(lua_State* L)
{
    Log("script: %s", luaL_checkstring(L, 1));
    return 0;
}

} // namespace

void RegisterApi()
{
    Register("fps", Fps);
    Register("scene", Scene);
    Register("resolution", Resolution);
    Register("log", LogFn);
    Register("uiscale", UiScaleFn);
    Register("freecam", FreeCam);
    Register("campos", CamPos);
    Register("setcampos", SetCamPos);
    Register("setcamrot", SetCamRot);
    Register("camrot", CamRot);
    Register("camlookat", CamLookAt);
    Register("speed", Speed);
    Register("god", God);
    Register("sound", Sound);
    Register("stopsound", StopSoundFn);
    Register("invisible", Invisible);
    Register("freeze", Freeze);
}

} // namespace h2::script
