// Lua access to scene objects. Handles identify objects by their stable path,
// so they stay valid while the scene is loaded. Changes made from scripts are
// runtime state (level logic), not level edits.
//
//   local crate = h2.find("Crate#2")            -- by name ("#k" = k-th of that name) or full path
//   for _, g in ipairs(h2.findall("Guard")) do ... end
//   for _, a in ipairs(h2.objects("ZActor")) do ... end   -- by class (incl. derived)
//   crate:name(), crate:class(), crate:path(), crate:isa("ZSTDOBJ"), crate:valid()
//   x, y, z = crate:pos();  crate:setpos(x, y + 100, z)
//   yaw, pitch, roll = crate:rot();  crate:setrot(90, 0, 0)
//   crate:hide(); crate:show(); crate:hidden()
#include "objects_api.h"

#include <cstdlib>
#include <string>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "../editor/model.h"
#include "lua_vm.h"

namespace h2::script {

namespace {

constexpr const char* kMeta = "h2.object";

void PushObject(lua_State* L, const std::string& path)
{
    lua_createtable(L, 0, 1);
    lua_pushlstring(L, path.data(), path.size());
    lua_setfield(L, -2, "_path");
    luaL_setmetatable(L, kMeta);
}

std::string PathArg(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "_path");
    std::string p = luaL_checkstring(L, -1);
    lua_pop(L, 1);
    return p;
}

engine::ZGEOM* Geom(lua_State* L)
{
    std::string path = PathArg(L);
    engine::ZGEOM* g = editor::Resolve(path);
    if (!g || !editor::IsLive(g))
        luaL_error(L, "object '%s' is not in the loaded scene", path.c_str());
    return g;
}

int Find(lua_State* L)
{
    std::string key = luaL_checkstring(L, 1);
    const auto& snap = editor::SceneModel();
    int i = snap.FindPath(key);
    if (i < 0) {
        std::string name = key;
        int k = 0;
        if (auto hash = key.rfind('#'); hash != std::string::npos && key.find('/') == std::string::npos) {
            name = key.substr(0, hash);
            k = std::atoi(key.c_str() + hash + 1);
        }
        auto found = snap.FindName(name);
        if (k < int(found.size()))
            i = found[k];
    }
    if (i < 0) {
        lua_pushnil(L);
        return 1;
    }
    PushObject(L, snap.geoms[i].path);
    return 1;
}

int FindAll(lua_State* L)
{
    std::string name = luaL_checkstring(L, 1);
    const auto& snap = editor::SceneModel();
    lua_newtable(L);
    int n = 0;
    for (int i : snap.FindName(name)) {
        PushObject(L, snap.geoms[i].path);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int Objects(lua_State* L)
{
    const char* cls = luaL_checkstring(L, 1);
    const auto& snap = editor::SceneModel();
    lua_newtable(L);
    int n = 0;
    for (const auto& g : snap.geoms) {
        if (!engine::IsA(g.geom, cls))
            continue;
        PushObject(L, g.path);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int Name(lua_State* L)
{
    const auto& snap = editor::SceneModel();
    int i = snap.Find(Geom(L));
    lua_pushstring(L, i >= 0 ? snap.geoms[i].name.c_str() : "");
    return 1;
}

int Class(lua_State* L)
{
    lua_pushstring(L, engine::ClassName(Geom(L)).c_str());
    return 1;
}

int Path(lua_State* L)
{
    lua_pushstring(L, PathArg(L).c_str());
    return 1;
}

int IsA(lua_State* L)
{
    lua_pushboolean(L, engine::IsA(Geom(L), luaL_checkstring(L, 2)));
    return 1;
}

int Valid(lua_State* L)
{
    engine::ZGEOM* g = editor::Resolve(PathArg(L));
    lua_pushboolean(L, g && editor::IsLive(g));
    return 1;
}

int Pos(lua_State* L)
{
    editor::Transform t = editor::GetTransform(Geom(L));
    for (float v : t.pos)
        lua_pushnumber(L, v);
    return 3;
}

int SetPos(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    editor::Transform t = editor::GetTransform(g);
    for (int i = 0; i < 3; ++i)
        t.pos[i] = float(luaL_checknumber(L, 2 + i));
    editor::SetTransform(g, t);
    return 0;
}

int Rot(lua_State* L)
{
    editor::Transform t = editor::GetTransform(Geom(L));
    float y, p, r;
    editor::MatrixToEuler(t.mat, y, p, r);
    lua_pushnumber(L, y);
    lua_pushnumber(L, p);
    lua_pushnumber(L, r);
    return 3;
}

int SetRot(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    editor::Transform t = editor::GetTransform(g);
    editor::EulerToMatrix(float(luaL_checknumber(L, 2)), float(luaL_optnumber(L, 3, 0)), float(luaL_optnumber(L, 4, 0)),
                          t.mat);
    editor::SetTransform(g, t);
    return 0;
}

int Hide(lua_State* L)
{
    editor::SetHidden(Geom(L), true);
    return 0;
}

int Show(lua_State* L)
{
    editor::SetHidden(Geom(L), false);
    return 0;
}

int Hidden(lua_State* L)
{
    lua_pushboolean(L, editor::IsHidden(Geom(L)));
    return 1;
}

int ToString(lua_State* L)
{
    lua_pushfstring(L, "object(%s)", PathArg(L).c_str());
    return 1;
}

int Eq(lua_State* L)
{
    std::string a = PathArg(L);
    lua_getfield(L, 2, "_path");
    const char* b = lua_tostring(L, -1);
    lua_pushboolean(L, b && a == b);
    return 1;
}

} // namespace

void RegisterObjectsApi()
{
    lua_State* L = State();
    const luaL_Reg methods[] = {
        {"name", Name},   {"class", Class},   {"path", Path}, {"isa", IsA},   {"valid", Valid},
        {"pos", Pos},     {"setpos", SetPos}, {"rot", Rot},   {"setrot", SetRot},
        {"hide", Hide},   {"show", Show},     {"hidden", Hidden},
        {nullptr, nullptr},
    };
    luaL_newmetatable(L, kMeta);
    luaL_newlib(L, methods);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, ToString);
    lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Eq);
    lua_setfield(L, -2, "__eq");
    lua_pop(L, 1);
    Register("find", Find);
    Register("findall", FindAll);
    Register("objects", Objects);
}

} // namespace h2::script
