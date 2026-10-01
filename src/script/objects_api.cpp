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
//
// Objects created by scripts (visual copies of an object's model, like the
// editor's; runtime only, not saved in the level, destroyed when the level
// scripts run again or the scene is left):
//   local c = crate:spawn()                       -- next to the original
//   local c = crate:spawn(x, y, z [, yaw])        -- at a position (turned by yaw degrees)
//   local c = h2.spawn("Crate_01", x, y, z [, yaw])  -- source by name/path or object
//   c:destroy();  c:spawned()  -- true for script objects
#include "objects_api.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "../editor/model.h"
#include "../engine/spawn.h"
#include "lua_vm.h"

namespace h2::script {

namespace {

constexpr const char* kMeta = "h2.object";

std::vector<engine::ZGEOM*> g_scriptObjects;  // created by scripts, alive
int g_spawnCounter;

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

// Object by full path, or name with optional "#k".
int FindIndex(const std::string& key)
{
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
    return i;
}

int Find(lua_State* L)
{
    int i = FindIndex(luaL_checkstring(L, 1));
    if (i < 0) {
        lua_pushnil(L);
        return 1;
    }
    PushObject(L, editor::SceneModel().geoms[i].path);
    return 1;
}

// Creates the copy of `source`; position arguments (x, y, z [, yaw]) from `arg`.
int SpawnFrom(lua_State* L, engine::ZGEOM* source, int arg)
{
    if (!engine::MeshOf(source) || editor::InActor(source))
        return luaL_error(L, "only objects with their own model can be copied (not characters or groups)");
    std::string base = engine::SafeString(source->name);
    std::string name = "Lua_" + (base.empty() ? std::string("Object") : base) + "_" + std::to_string(++g_spawnCounter);
    std::string err;
    engine::ZGEOM* g = editor::SpawnClone(source, name, &err);
    if (!g)
        return luaL_error(L, "spawn failed: %s", err.c_str());
    g_scriptObjects.push_back(g);
    editor::Transform t = editor::GetTransform(source);
    if (lua_gettop(L) >= arg + 2) {
        for (int i = 0; i < 3; ++i)
            t.pos[i] = float(luaL_checknumber(L, arg + i));
        if (!lua_isnoneornil(L, arg + 3))
            editor::EulerToMatrix(float(luaL_checknumber(L, arg + 3)), 0.0f, 0.0f, t.mat);
    } else {
        float c[3], r = 100.0f;
        if (!editor::GetBounds(source, c, r))
            r = 100.0f;
        t.pos[0] += r < 50.0f ? 50.0f : (r > 500.0f ? 500.0f : r);
    }
    editor::SetTransform(g, t);
    PushObject(L, editor::PathOf(g));
    return 1;
}

// h2.item(template, x, y, z [, yaw]): a working item (weapon, ammo, key...)
// made from an item template of the scene, e.g. h2.item("RifleSniperR93", x, y, z)
int Item(lua_State* L)
{
    engine::ZGEOM* tmpl = nullptr;
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "_path");
        tmpl = editor::Resolve(luaL_checkstring(L, -1));
        lua_pop(L, 1);
    } else {
        int i = FindIndex(luaL_checkstring(L, 1));
        tmpl = i >= 0 ? editor::SceneModel().geoms[i].geom : nullptr;
    }
    if (!tmpl || !editor::IsLive(tmpl) || !engine::IsA(tmpl, "ZItemTemplate"))
        return luaL_error(L, "item: no such item template (see h2.objects(\"ZItemTemplate\"))");
    std::string err;
    engine::ZGEOM* item = editor::SpawnItem(tmpl, &err);
    if (!item)
        return luaL_error(L, "item failed: %s", err.c_str());
    g_scriptObjects.push_back(item);
    editor::Transform t = editor::GetTransform(item);
    for (int i = 0; i < 3; ++i)
        t.pos[i] = float(luaL_checknumber(L, 2 + i));
    if (!lua_isnoneornil(L, 5))
        editor::EulerToMatrix(float(luaL_checknumber(L, 5)), 0.0f, 0.0f, t.mat);
    editor::SetTransform(item, t);
    PushObject(L, editor::PathOf(item));
    return 1;
}

int Spawn(lua_State* L)
{
    engine::ZGEOM* source = nullptr;
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "_path");
        source = editor::Resolve(luaL_checkstring(L, -1));
        lua_pop(L, 1);
    } else {
        int i = FindIndex(luaL_checkstring(L, 1));
        source = i >= 0 ? editor::SceneModel().geoms[i].geom : nullptr;
    }
    if (!source || !editor::IsLive(source))
        return luaL_error(L, "spawn: source object not found");
    return SpawnFrom(L, source, 2);
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

int ObjSpawn(lua_State* L) { return SpawnFrom(L, Geom(L), 2); }

int Destroy(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    auto it = std::find(g_scriptObjects.begin(), g_scriptObjects.end(), g);
    if (it == g_scriptObjects.end())
        return luaL_error(L, "only objects created by scripts can be destroyed (hide() the others)");
    g_scriptObjects.erase(it);
    editor::DestroySpawned(g);
    return 0;
}

int Spawned(lua_State* L)
{
    engine::ZGEOM* g = editor::Resolve(PathArg(L));
    lua_pushboolean(L, g && std::find(g_scriptObjects.begin(), g_scriptObjects.end(), g) != g_scriptObjects.end());
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
        {"spawn", ObjSpawn}, {"destroy", Destroy}, {"spawned", Spawned},
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
    Register("spawn", Spawn);
    Register("item", Item);
}

void DestroyScriptObjects()
{
    for (engine::ZGEOM* g : g_scriptObjects)
        if (editor::IsSpawned(g))
            editor::DestroySpawned(g);
    g_scriptObjects.clear();
}

void ForgetScriptObjects() { g_scriptObjects.clear(); }

} // namespace h2::script
