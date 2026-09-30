#include "api.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "imgui.h"

#include "../display.h"
#include "../editor/freecam.h"
#include "../log.h"
#include "../scene.h"
#include "../ui.h"
#include "lua_vm.h"

namespace h2::script {

namespace {

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

// h2.campos() -> x, y, z (camera position, centimetres, +Y up)
int CamPos(lua_State* L)
{
    float p[3];
    editor::FreeCamPosition(p);
    for (float v : p)
        lua_pushnumber(L, v);
    return 3;
}

// h2.setcampos(x, y, z): moves the free camera
int SetCamPos(lua_State* L)
{
    float p[3] = {float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)), float(luaL_checknumber(L, 3))};
    editor::SetFreeCamPosition(p);
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
}

} // namespace h2::script
