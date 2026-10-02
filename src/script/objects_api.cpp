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
//
// Routes (lists of points: patrol routes, safe points...; runtime changes):
//   for i, p in ipairs(route:points()) do ... end
//   route:setpoints({ h2.find("WP1"), h2.point(x, y, z), ... })
//   local p = h2.point(x, y, z [, room])   -- an invisible point (default room: ROOT)
//
// Characters and the mission:
//   npc:alive(), npc:kill()
//   h2.player()                            -- Hitman
//   h2.objective(slot [, text [, done]])   -- a laptop objective line; returns the level's own text
//   h2.mission("complete" | "fail")
#include "objects_api.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "../editor/mission.h"
#include "../editor/model.h"
#include "../editor/npcs.h"
#include "../editor/scene_panel.h"
#include "../engine/lists.h"
#include "../engine/messages.h"
#include "../engine/npc.h"
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

// ---- NPCs ----

// Lua value -> object ref for a ZGEOMREF parameter: object, name/path, or nil/false
uint32_t RefArg(lua_State* L, int idx)
{
    if (lua_isnoneornil(L, idx) || (lua_isboolean(L, idx) && !lua_toboolean(L, idx)))
        return 0;
    engine::ZGEOM* g = nullptr;
    if (lua_istable(L, idx)) {
        lua_getfield(L, idx, "_path");
        g = editor::Resolve(luaL_checkstring(L, -1));
        lua_pop(L, 1);
    } else {
        int i = FindIndex(luaL_checkstring(L, idx));
        g = i >= 0 ? editor::SceneModel().geoms[i].geom : nullptr;
    }
    if (!g)
        luaL_error(L, "parameter object not found");
    return engine::RefOf(g);
}

// Applies a Lua table {Param = value, ...} onto a behaviour.
void ApplyParams(lua_State* L, int idx, engine::ScriptInfo& s)
{
    if (lua_isnoneornil(L, idx))
        return;
    luaL_checktype(L, idx, LUA_TTABLE);
    for (engine::ScriptParam& p : s.params) {
        lua_getfield(L, idx, p.name.c_str());
        int top = lua_gettop(L);
        if (!lua_isnil(L, top)) {
            switch (p.type) {
            case engine::ParamType::Long:
                if (lua_isboolean(L, top))
                    p.bits = lua_toboolean(L, top) ? 1u : 0u;
                else
                    p.bits = uint32_t(luaL_checkinteger(L, top));
                break;
            case engine::ParamType::Float: {
                float f = float(luaL_checknumber(L, top));
                memcpy(&p.bits, &f, 4);
                break;
            }
            case engine::ParamType::Ref:
                p.bits = RefArg(L, top);
                break;
            case engine::ParamType::String:
                p.text = luaL_checkstring(L, top);
                break;
            default:
                break;
            }
        }
        lua_pop(L, 1);
    }
}

void PushParams(lua_State* L, const engine::ScriptInfo& s)
{
    lua_newtable(L);
    for (const engine::ScriptParam& p : s.params) {
        switch (p.type) {
        case engine::ParamType::Long:
            lua_pushinteger(L, int(p.bits));
            break;
        case engine::ParamType::Float: {
            float f;
            memcpy(&f, &p.bits, 4);
            lua_pushnumber(L, f);
            break;
        }
        case engine::ParamType::Ref:
            if (engine::ZGEOM* g = p.bits ? engine::GeomOfRef(p.bits) : nullptr)
                PushObject(L, editor::PathOf(g));
            else
                lua_pushboolean(L, 0);
            break;
        case engine::ParamType::String:
            lua_pushstring(L, p.text.c_str());
            break;
        default:
            continue;
        }
        lua_setfield(L, -2, p.name.c_str());
    }
}

// h2.npcs() -> { {record=, model=, behaviour=, object=}, ... } characters of the scene
int Npcs(lua_State* L)
{
    lua_newtable(L);
    int n = 0;
    for (int r : editor::NpcRecords()) {
        lua_newtable(L);
        lua_pushinteger(L, r);
        lua_setfield(L, -2, "record");
        lua_pushstring(L, editor::NpcModel(r).c_str());
        lua_setfield(L, -2, "model");
        engine::ScriptInfo s;
        if (editor::AuthoredBehaviour(r, s)) {
            lua_pushstring(L, engine::ScriptShortName(s.name).c_str());
            lua_setfield(L, -2, "behaviour");
        }
        if (engine::ZGEOM* g = engine::RecordGeom(r)) {
            PushObject(L, editor::PathOf(g));
            lua_setfield(L, -2, "object");
        }
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// h2.player() -> Hitman
int Player(lua_State* L)
{
    engine::ZGEOM* g = engine::Player();
    if (!g) {
        lua_pushnil(L);
        return 1;
    }
    PushObject(L, editor::PathOf(g));
    return 1;
}

// h2.objective(slot [, text [, done]]) -> the level's own text of the line
int Objective(lua_State* L)
{
    int slot = int(luaL_checkinteger(L, 1));
    if (slot < 1 || slot > 6)
        return luaL_error(L, "objective: slot 1..6");
    std::string own = editor::LevelObjectiveText(slot);
    if (lua_isstring(L, 2))
        editor::SetObjectiveText(slot, lua_tostring(L, 2));
    if (lua_isboolean(L, 3))
        editor::SetObjectiveDone(slot, lua_toboolean(L, 3) != 0);
    lua_pushstring(L, own.c_str());
    return 1;
}

// h2.mission("complete" | "fail")
int Mission(lua_State* L)
{
    std::string op = luaL_checkstring(L, 1);
    bool ok = op == "complete" ? editor::CompleteMission()
              : op == "fail"   ? editor::FailMission()
                               : (luaL_error(L, "mission: complete or fail"), false);
    lua_pushboolean(L, ok);
    return 1;
}

// h2.point(x, y, z [, room]) -> object: an invisible point characters can be sent to
int Point(lua_State* L)
{
    editor::Transform t = {{0, 0, 1, 0, 1, 0, 1, 0, 0}, {0, 0, 0}};
    for (int i = 0; i < 3; ++i)
        t.pos[i] = float(luaL_checknumber(L, i + 1));
    engine::ZGEOM* room = nullptr;
    if (lua_istable(L, 4)) {
        lua_getfield(L, 4, "_path");
        const char* path = lua_tostring(L, -1);
        room = path ? editor::Resolve(path) : nullptr;
        lua_pop(L, 1);
    } else {
        room = editor::Resolve("ROOT");
    }
    if (!room || !editor::IsLive(room))
        return luaL_error(L, "point: no room");
    std::string err;
    engine::ZGEOM* g =
        editor::SpawnPoint("ScriptPoint_" + std::to_string(++g_spawnCounter), room, t, &err);
    if (!g)
        return luaL_error(L, "point: %s", err.c_str());
    PushObject(L, editor::PathOf(g));
    return 1;
}

// h2.behaviours() -> { "BaseGuard", ... } behaviours (script classes) of the scene
int Behaviours(lua_State* L)
{
    lua_newtable(L);
    int n = 0;
    for (const std::string& b : editor::BehaviourNames()) {
        lua_pushstring(L, b.c_str());
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// h2.npc(who, x, y, z [, yaw [, behaviour [, params]]]) -> object
//   who: model name ("BodyguardMafia_01"), group name, record number or a scene NPC object
//   behaviour: script name ("BaseGuard"), false for none, nil for the character's own
//   params: { GuardPoint = h2.find("Lobby"), IsSentry = 1, ... }
int Npc(lua_State* L)
{
    int record = -1;
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "_path");
        record = editor::FindNpcRecord(luaL_checkstring(L, -1));
        lua_pop(L, 1);
    } else if (lua_isinteger(L, 1)) {
        record = editor::FindNpcRecord(std::to_string(lua_tointeger(L, 1)));
    } else {
        record = editor::FindNpcRecord(luaL_checkstring(L, 1));
    }
    if (record < 0)
        return luaL_error(L, "npc: no such character (see h2.npcs())");
    editor::NpcSpec spec;
    spec.record = record;
    editor::EulerToMatrix(float(luaL_optnumber(L, 5, 0.0)), 0.0f, 0.0f, spec.transform.mat);
    for (int i = 0; i < 3; ++i)
        spec.transform.pos[i] = float(luaL_checknumber(L, 2 + i));
    if (lua_isboolean(L, 6) && !lua_toboolean(L, 6)) {
        spec.hasBehaviour = false;
    } else if (lua_isnoneornil(L, 6)) {
        spec.hasBehaviour = editor::AuthoredBehaviour(record, spec.behaviour);
    } else {
        if (!editor::BehaviourTemplate(luaL_checkstring(L, 6), spec.behaviour))
            return luaL_error(L, "npc: no behaviour '%s' in this scene (see h2.behaviours())", lua_tostring(L, 6));
        spec.hasBehaviour = true;
    }
    ApplyParams(L, 7, spec.behaviour);
    std::string err;
    engine::ZGEOM* g = editor::CreateNpc(spec, &err);
    if (!g)
        return luaL_error(L, "npc failed: %s", err.c_str());
    PushObject(L, editor::PathOf(g));
    return 1;
}

// obj:behaviour() -> name, params | nothing ; obj:behaviour(name|false [, params]) changes it
int Behaviour(lua_State* L)
{
    engine::ZGEOM* actor = editor::ActorOf(Geom(L));
    if (!actor)
        return luaL_error(L, "not a character");
    if (lua_gettop(L) == 1) {
        engine::ScriptInfo s;
        if (!editor::CurrentBehaviour(actor, s))
            return 0;
        lua_pushstring(L, engine::ScriptShortName(s.name).c_str());
        PushParams(L, s);
        return 2;
    }
    std::string err;
    if (lua_isboolean(L, 2) && !lua_toboolean(L, 2)) {
        if (!editor::SetNpcBehaviour(actor, nullptr, &err))
            return luaL_error(L, "behaviour: %s", err.c_str());
        return 0;
    }
    engine::ScriptInfo s;
    std::string name = luaL_checkstring(L, 2);
    engine::ScriptInfo current;
    bool same = editor::CurrentBehaviour(actor, current) &&
                engine::ScriptShortName(current.name) == engine::ScriptShortName(name);
    if (same)
        s = current;  // keep the other parameters
    else if (!editor::BehaviourTemplate(name, s))
        return luaL_error(L, "behaviour: no '%s' in this scene", name.c_str());
    ApplyParams(L, 3, s);
    if (!editor::SetNpcBehaviour(actor, &s, &err))
        return luaL_error(L, "behaviour: %s", err.c_str());
    return 0;
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

// obj:state() -> name of the state the character's behaviour is in
int State(lua_State* L)
{
    engine::ZGEOM* actor = editor::ActorOf(Geom(L));
    std::string s = actor ? engine::ScriptState(actor) : std::string();
    if (s.empty())
        return 0;
    lua_pushstring(L, s.c_str());
    return 1;
}

// obj:children() -> { objects } ; obj:parent() -> object | nil
int Children(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    const auto& snap = editor::SceneModel(0);
    int i = snap.Find(g);
    lua_newtable(L);
    if (i < 0)
        return 1;
    int n = 0;
    for (int c : snap.geoms[i].children) {
        PushObject(L, snap.geoms[c].path);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int Parent(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    engine::ZBaseGeom* p = g->node->parent;
    if (!p || !engine::IsReadable(p, sizeof(*p)) || !p->geom || !editor::IsLive(p->geom))
        return 0;
    PushObject(L, editor::PathOf(p->geom));
    return 1;
}

// obj:select(): selects the object in the editor
// npc:alive() -> bool
int Alive(lua_State* L)
{
    lua_pushboolean(L, engine::IsAlive(Geom(L)));
    return 1;
}

// npc:kill() -> bool (killed now)
int Kill(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    if (!engine::IsA(g, "ZActor"))
        return luaL_error(L, "kill: %s is not a character", engine::SafeString(g->name).c_str());
    lua_pushboolean(L, engine::KillActor(g));
    return 1;
}

// list:points() -> { object, ... } its elements in order
int Points(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    if (!engine::IsEditableList(g))
        return luaL_error(L, "points: %s is not a list of points", engine::SafeString(g->name).c_str());
    lua_newtable(L);
    int n = 0;
    for (uint32_t r : engine::ListRefs(g)) {
        engine::ZGEOM* p = engine::GeomOfRef(r);
        if (!p || !editor::IsLive(p))
            continue;
        PushObject(L, editor::PathOf(p));
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// list:setpoints({ object, ... })
int SetPoints(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<uint32_t> refs;
    for (lua_Integer i = 1;; ++i) {
        if (lua_rawgeti(L, 2, i) == LUA_TNIL) {
            lua_pop(L, 1);
            break;
        }
        lua_getfield(L, -1, "_path");
        const char* path = lua_tostring(L, -1);
        engine::ZGEOM* p = path ? editor::Resolve(path) : nullptr;
        if (!p || !editor::IsLive(p))
            return luaL_error(L, "setpoints: element %d is not an object of the scene", int(i));
        refs.push_back(engine::RefOf(p));
        lua_pop(L, 2);
    }
    std::string err;
    if (!engine::SetListRefs(g, refs, &err))
        return luaL_error(L, "setpoints: %s", err.c_str());
    return 0;
}

int SelectObj(lua_State* L)
{
    editor::Select(Geom(L));
    return 0;
}

// obj:addr() -> geom address, node address (reverse engineering)
int Addr(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    lua_pushinteger(L, lua_Integer(reinterpret_cast<uintptr_t>(g)));
    lua_pushinteger(L, lua_Integer(reinterpret_cast<uintptr_t>(g->node)));
    return 2;
}

// obj:flags() -> node flags ; obj:mesh() -> mesh ref (0: none)
int Flags(lua_State* L)
{
    lua_pushinteger(L, lua_Integer(Geom(L)->node->flags));
    return 1;
}

int Mesh(lua_State* L)
{
    lua_pushinteger(L, lua_Integer(engine::MeshOf(Geom(L))));
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
        {"spawn", ObjSpawn}, {"destroy", Destroy}, {"spawned", Spawned}, {"behaviour", Behaviour}, {"state", State},
        {"children", Children}, {"parent", Parent}, {"flags", Flags}, {"mesh", Mesh}, {"addr", Addr}, {"select", SelectObj},
        {"points", Points}, {"setpoints", SetPoints}, {"alive", Alive}, {"kill", Kill},
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
    Register("npcs", Npcs);
    Register("behaviours", Behaviours);
    Register("npc", Npc);
    Register("point", Point);
    Register("player", Player);
    Register("objective", Objective);
    Register("mission", Mission);
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
