#include "events.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "lua_vm.h"

namespace h2::script {

namespace {

constexpr const char* kHandlers = "h2.handlers";  // registry: name -> { fn, ... }

// h2.on(name, fn)
int On(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_getfield(L, LUA_REGISTRYINDEX, kHandlers);
    if (lua_getfield(L, -1, name) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, name);
    }
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, lua_rawlen(L, -2) + 1);
    return 0;
}

// h2.off(name): removes every handler of the event
int Off(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, kHandlers);
    lua_pushnil(L);
    lua_setfield(L, -2, name);
    return 0;
}

// Calls every handler of `name` with the values pushed by `push`.
template <typename Push>
void Fire(const char* name, Push push)
{
    lua_State* L = State();
    if (!L)
        return;
    lua_getfield(L, LUA_REGISTRYINDEX, kHandlers);
    if (lua_getfield(L, -1, name) != LUA_TTABLE) {
        lua_pop(L, 2);
        return;
    }
    lua_Integer n = lua_rawlen(L, -1);
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, i);
        int nargs = push(L);
        if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
            ConsolePrint(LineKind::Error, std::string(name) + " handler: " + lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 2);
}

} // namespace

void RegisterEvents()
{
    lua_State* L = State();
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kHandlers);
    Register("on", On);
    Register("off", Off);
}

void FireScene(const std::string& scene)
{
    Fire("scene", [&](lua_State* L) {
        lua_pushstring(L, scene.c_str());
        return 1;
    });
}

void FireFrame(double dt)
{
    Fire("frame", [&](lua_State* L) {
        lua_pushnumber(L, dt);
        return 1;
    });
}

} // namespace h2::script
