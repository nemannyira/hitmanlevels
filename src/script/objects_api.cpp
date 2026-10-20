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
//   npc:walkto(x, y, z) / npc:walkto(obj), npc:lookat(obj | nil), npc:moveset("Run"),
//   npc:animation("Stand_Relaxed_Alt01" [, loop]), npc:faceto(obj | x, y, z)
//     (as the scripts' MoveToPosition / LookAt / UseMoveSet; a character's
//     own behaviour may give it other orders: place it without one; it
//     walks only to points of the level's path network)
//   h2.player()                            -- Hitman
//   h2.objective(slot [, text [, done]])   -- a laptop objective line; returns the level's own text
//   h2.objective(slot, false)              -- hides a line (the level's own too)
//   h2.briefing(text)                      -- the laptop's mission briefing ("" = the level's own)
//   h2.mission("complete" | "fail")
//   h2.subtitle(text [, seconds])          -- a subtitle line (triggers.cpp)
//   h2.basemission(false)                  -- the base level's own logic no longer ends the mission
//
// Music:
//   h2.music() -> { {object=, zone=, mood="Location"|"PreAction"|"Action", track=, level=}, ... }, { "RussiaSlow", ... }
//   h2.music(object, "RussiaSlow")         -- another track ("" = the level's own; runtime)
#include "objects_api.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <windows.h>
#include <cctype>
#include <cmath>
#include <map>
#include <unordered_map>
#include <string>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "../editor/level.h"
#include "../editor/mission.h"
#include "../editor/model.h"
#include "../editor/music.h"
#include "../editor/npcs.h"
#include "../editor/scene_panel.h"
#include "../engine/lists.h"
#include "../engine/messages.h"
#include "../engine/npc.h"
#include "../engine/scene_data.h"
#include "../engine/spawn.h"
#include "../engine/game_events.h"
#include "../engine/imports.h"
#include "../log.h"
#include "lua_vm.h"

namespace h2::script {

namespace {

// a number of a position or angle from a script: finite and in the world
float Coordinate(lua_State* L, int arg)
{
    double v = luaL_checknumber(L, arg);
    if (!std::isfinite(v) || std::fabs(v) > 1e7)
        luaL_argerror(L, arg, "not a usable coordinate");
    return float(v);
}

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
    if (i < 0 && key.find('/') != std::string::npos)  // an object of the level in another room after a load (Resolve)
        if (engine::ZGEOM* g = editor::Resolve(key))
            i = snap.Find(g);
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
    editor::Transform t = editor::GetTransform(source);
    bool placed = lua_gettop(L) >= arg + 2;
    if (placed) {
        for (int i = 0; i < 3; ++i)
            t.pos[i] = Coordinate(L, arg + i);
        if (!lua_isnoneornil(L, arg + 3))
            editor::EulerToMatrix(Coordinate(L, arg + 3), 0.0f, 0.0f, t.mat);
    }
    // in the room of the place it goes to: the game draws a room's objects only where that room
    // is seen (a copy of a garage lamp left in the garage room stayed unseen on the road)
    std::string err;
    engine::ZGEOM* g = editor::SpawnClone(source, name, &err, placed ? editor::RoomAt(t.pos) : nullptr);
    if (!g)
        return luaL_error(L, "spawn failed: %s", err.c_str());
    editor::UseBlankedMesh(engine::MeshOf(g));
    g_scriptObjects.push_back(g);
    if (!placed) {
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
        t.pos[i] = Coordinate(L, 2 + i);
    if (!lua_isnoneornil(L, 5))
        editor::EulerToMatrix(Coordinate(L, 5), 0.0f, 0.0f, t.mat);
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

// h2.sky([level]) -> level: the sky of another level ("C3-3"; "" = the map's own;
// saved with the level, the map is loaded again for a sky not imported yet)
int Sky(lua_State* L)
{
    if (lua_gettop(L) >= 1) {
        std::string level = luaL_checkstring(L, 1);
        const auto& levels = editor::SkyLevels();
        auto it = std::find_if(levels.begin(), levels.end(),
                               [&](const std::string& l) { return _stricmp(l.c_str(), level.c_str()) == 0; });
        if (!level.empty() && it == levels.end())
            return luaL_error(L, "sky: no level %s with a sky", level.c_str());
        editor::SetSky(level.empty() ? "" : *it);
    }
    lua_pushstring(L, editor::CurrentLevel().skyLevel.c_str());
    return 1;
}

// h2.briefing(text): the laptop's mission briefing until the map loads again
int Briefing(lua_State* L)
{
    lua_pushboolean(L, editor::SetBriefingText(luaL_checkstring(L, 1)));
    return 1;
}

// h2.objective(slot [, text [, done]]) -> the level's own text of the line;
// h2.objective(slot, false) hides the line (a level's own too)
int Objective(lua_State* L)
{
    int slot = int(luaL_checkinteger(L, 1));
    if (slot < 1 || slot > 6)
        return luaL_error(L, "objective: slot 1..6");
    std::string own = editor::LevelObjectiveText(slot);
    if (lua_isboolean(L, 2) && !lua_toboolean(L, 2)) {  // h2.objective(slot, false): hidden
        editor::HideObjectiveLine(slot);
        lua_pushstring(L, own.c_str());
        return 1;
    }
    if (!lua_isnoneornil(L, 2) && !lua_isstring(L, 2))
        return luaL_argerror(L, 2, "a text, nil or false");
    if (lua_isstring(L, 2))
        editor::SetObjectiveText(slot, lua_tostring(L, 2));
    if (lua_isboolean(L, 3))
        editor::SetObjectiveDone(slot, lua_toboolean(L, 3) != 0);
    lua_pushstring(L, own.c_str());
    return 1;
}

// h2.music() -> slots, tracks | h2.music(object, track)
int Music(lua_State* L)
{
    if (lua_gettop(L) >= 2) {
        lua_getfield(L, 1, "_path");
        const char* path = lua_tostring(L, -1);
        std::string err;
        if (!path || !editor::SetMusicTrack(path, luaL_checkstring(L, 2), &err))
            return luaL_error(L, "music: %s", path ? err.c_str() : "object expected");
        return 0;
    }
    lua_newtable(L);
    int n = 0;
    for (const editor::MusicSlot& s : editor::MusicSlots()) {
        lua_newtable(L);
        PushObject(L, s.path);
        lua_setfield(L, -2, "object");
        lua_pushstring(L, s.zone.c_str());
        lua_setfield(L, -2, "zone");
        lua_pushstring(L, s.mood.c_str());
        lua_setfield(L, -2, "mood");
        lua_pushstring(L, editor::TrackName(s.current).c_str());
        lua_setfield(L, -2, "track");
        lua_pushstring(L, editor::TrackName(s.original).c_str());
        lua_setfield(L, -2, "level");
        lua_rawseti(L, -2, ++n);
    }
    lua_newtable(L);
    n = 0;
    for (const editor::MusicTrack& t : editor::MusicTracks()) {
        lua_pushstring(L, t.name.c_str());
        lua_rawseti(L, -2, ++n);
    }
    return 2;
}

// h2.basemission([on]) -> on: whether the base level's own logic may end the
// mission (the level's setting until the map is left)
int BaseMission(lua_State* L)
{
    if (!lua_isnoneornil(L, 1))
        engine::SetLevelEndsAllowed(lua_toboolean(L, 1) != 0);
    lua_pushboolean(L, engine::LevelEndsAllowed());
    return 1;
}

// h2.onpath(x, y, z) / h2.onpath(object) -> bool: whether a character can
// walk to the point (the map's path network is near it); nil: the map has
// no network
int OnPath(lua_State* L)
{
    float pos[3];
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "_path");
        engine::ZGEOM* t = editor::Resolve(luaL_checkstring(L, -1));
        lua_pop(L, 1);
        if (!t || !editor::IsLive(t))
            return luaL_error(L, "onpath: the object is not in the loaded scene");
        engine::WorldPosition(t, pos);
    } else {
        for (int i = 0; i < 3; ++i)
            pos[i] = Coordinate(L, 1 + i);
    }
    int on = engine::OnPathNetwork(pos);
    if (on < 0)
        lua_pushnil(L);
    else
        lua_pushboolean(L, on);
    return 1;
}

// h2.walkable(x, y, z [, radius]) / h2.walkable(object [, radius]) -> x, y, z:
// the nearest point of the path network on about that floor (radius: across,
// default 2 m, at most 8 m); nil when there is none that near (or no network)
int Walkable(lua_State* L)
{
    float pos[3];
    int next;
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "_path");
        engine::ZGEOM* t = editor::Resolve(luaL_checkstring(L, -1));
        lua_pop(L, 1);
        if (!t || !editor::IsLive(t))
            return luaL_error(L, "walkable: the object is not in the loaded scene");
        engine::WorldPosition(t, pos);
        next = 2;
    } else {
        for (int i = 0; i < 3; ++i)
            pos[i] = Coordinate(L, 1 + i);
        next = 4;
    }
    lua_Number radius = luaL_optnumber(L, next, 200.0);
    if (!(radius > 0.0) || radius > 800.0)
        return luaL_error(L, "walkable: radius between 0 and 800 cm");
    float out[3];
    if (!engine::NearestWalkable(pos, float(radius), out)) {
        lua_pushnil(L);
        return 1;
    }
    for (float v : out)
        lua_pushnumber(L, v);
    return 3;
}

// h2.give(item | template [, character]) -> item: the item goes to the
// character (default Hitman) as if picked up; a template name makes the item
// first (where the character is)
int Give(lua_State* L)
{
    engine::ZGEOM* owner = engine::Player();
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "_path");
        owner = editor::Resolve(luaL_checkstring(L, -1));
        lua_pop(L, 1);
        if (owner && editor::ActorOf(owner))
            owner = editor::ActorOf(owner);
    }
    if (!owner || !editor::IsLive(owner) || !(engine::IsA(owner, "ZActor") || engine::IsA(owner, "ZHITMAN2")))
        return luaL_error(L, "give: no such character");
    engine::ZGEOM* item = nullptr;
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "_path");
        item = editor::Resolve(luaL_checkstring(L, -1));
        lua_pop(L, 1);
        if (!item || !editor::IsLive(item))
            return luaL_error(L, "give: the item is not in the loaded scene");
    } else {
        int i = FindIndex(luaL_checkstring(L, 1));
        engine::ZGEOM* tmpl = i >= 0 ? editor::SceneModel().geoms[i].geom : nullptr;
        if (!tmpl || !editor::IsLive(tmpl) || !engine::IsA(tmpl, "ZItemTemplate"))
            return luaL_error(L, "give: no such item template (see h2.objects(\"ZItemTemplate\"))");
        // ammunition counts only when picked up (the item handed over adds nothing)
        if (engine::SafeString(tmpl->name).rfind("Ammo", 0) == 0)
            return luaL_error(L, "give: ammunition is added only when Hitman picks it up: lay it down "
                                 "with h2.item(\"%s\", x, y, z)",
                              engine::SafeString(tmpl->name).c_str());
        // (A weapon given while copies of it lay on the map took the game down when drawn (P5,
        // 2026-10-12): the game's loop placing a weapon's parts used a dead part reference; the
        // editor sends that loop on (engine/npc.cpp kPartsNext), seen in the game 2026-10-19.)
        std::string err;
        item = editor::SpawnItem(tmpl, &err);
        if (!item)
            return luaL_error(L, "give: %s", err.c_str());
        g_scriptObjects.push_back(item);
        editor::Transform t = editor::GetTransform(item);
        editor::Transform o = editor::GetTransform(owner);
        memcpy(t.pos, o.pos, sizeof(t.pos));
        editor::SetTransform(item, t);
    }
    if (!engine::GiveItem(item, owner))
        return luaL_error(L, "give: the item could not be given");
    PushObject(L, editor::PathOf(item));
    return 1;
}

// h2.health([points]) -> Hitman's hit points (ZHITMAN2 +0xD72, float); a new
// value above 0 (to kill him: h2.mission("fail") or the game's own ways)
int Health(lua_State* L)
{
    engine::ZGEOM* hitman = engine::Player();
    char* hp = hitman ? reinterpret_cast<char*>(hitman) + 0xD72 : nullptr;
    if (!hp || !engine::IsReadable(hp, 4))
        return 0;
    if (!lua_isnoneornil(L, 1)) {
        float v = Coordinate(L, 1);
        if (!(v > 0.0f) || v > 10000.0f)
            return luaL_error(L, "health: above 0 and at most 10000");
        *reinterpret_cast<float*>(hp) = v;
        float max = *reinterpret_cast<float*>(hp + 4);  // +0xD76
        if (max > 0.0f)
            editor::ShowHitPoints(int(std::min(100.0f, 100.0f / max * v)));
    }
    lua_pushnumber(L, *reinterpret_cast<float*>(hp));
    return 1;
}

// h2.capacity() -> how many more objects the map can take (nodes left in
// the engine's buffer; a character takes about 15)
int Capacity(lua_State* L)
{
    uint32_t largest = 0;
    lua_pushinteger(L, engine::FreeNodeSlots());
    lua_pushinteger(L, lua_Integer(engine::FreeObjectBytes(&largest)));
    lua_pushinteger(L, largest);
    return 3;
}

// h2.disguise(character | clothes item) -> sent: Hitman takes its clothes
int Disguise(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "_path");
    engine::ZGEOM* g = editor::Resolve(luaL_checkstring(L, -1));
    lua_pop(L, 1);
    if (!g || !editor::IsLive(g))
        return luaL_error(L, "disguise: the object is not in the loaded scene");
    if (engine::ZGEOM* a = editor::ActorOf(g))
        g = a;
    lua_pushboolean(L, engine::HitmanChangeClothes(g));
    return 1;
}

// h2._subtitle(text): the subtitle line now (h2.subtitle in triggers.cpp
// takes it away after some seconds)
int Subtitle(lua_State* L)
{
    lua_pushboolean(L, editor::ShowSubtitle(luaL_optstring(L, 1, "")));
    return 1;
}

// h2.info(text [, "info" | "warning" | "hint"]) -> shown: the game's info box
int Info(lua_State* L)
{
    const char* text = luaL_checkstring(L, 1);
    static const char* const kinds[] = {"info", "warning", "hint", nullptr};
    int kind = luaL_checkoption(L, 2, "info", kinds);
    lua_pushboolean(L, editor::ShowInfo(static_cast<editor::InfoKind>(kind), text));
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
        t.pos[i] = Coordinate(L, i + 1);
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

// h2.behaviours([others]) -> { "BaseGuard", ... } behaviours placed characters can have
// (others: also those of the other levels, "Guard2_1 (C2-1)")
int Behaviours(lua_State* L)
{
    bool others = lua_toboolean(L, 1);  // before the table: it would be index 1 without an argument
    lua_newtable(L);
    int n = 0;
    for (const std::string& b : editor::BehaviourNames()) {
        lua_pushstring(L, b.c_str());
        lua_rawseti(L, -2, ++n);
    }
    if (others)
        for (const std::string& b : editor::ForeignBehaviourNames()) {
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
    editor::EulerToMatrix(lua_isnoneornil(L, 5) ? 0.0f : Coordinate(L, 5), 0.0f, 0.0f, spec.transform.mat);
    for (int i = 0; i < 3; ++i)
        spec.transform.pos[i] = Coordinate(L, 2 + i);
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
    spec.now = true;  // the game runs: it acts at once
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

// h2.import_models{ {"C2-1", 0x33DFFE}, ... }: models of other levels in the
// scenes loaded from now on (see docs/models.md); h2.import_models{} clears.
int ImportModels(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<engine::ModelImport> list;
    lua_Integer n = luaL_len(L, 1);
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_geti(L, 1, i);
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_geti(L, -1, 1);
        lua_geti(L, -2, 2);
        engine::ModelImport m;
        m.level = luaL_checkstring(L, -2);
        m.mesh = uint32_t(luaL_checkinteger(L, -1));
        list.push_back(m);
        lua_pop(L, 3);
    }
    engine::SetModelImports(list);
    return 0;
}

// h2.imported_mesh(level, mesh): its mesh reference in the loaded scene (nil: not imported)
int ImportedMesh(lua_State* L)
{
    uint32_t m = engine::ImportedMesh(luaL_checkstring(L, 1), uint32_t(luaL_checkinteger(L, 2)));
    if (!m)
        return 0;
    lua_pushinteger(L, m);
    return 1;
}

// h2.mesh_object(mesh, x, y, z [, parent [, flags]]): a plain object showing a
// mesh of the loaded scene (default parent: Hitman's room)
int MeshObject(lua_State* L)
{
    uint32_t mesh = uint32_t(luaL_checkinteger(L, 1));
    float pos[3] = {Coordinate(L, 2), Coordinate(L, 3), Coordinate(L, 4)};
    if (!engine::IsPlainModel(mesh))  // the renderer reads it as a model record: only real ones
        return luaL_error(L, "mesh_object: %d is not a model of this map (h2.imported_mesh)", int(mesh));
    engine::ZGEOM* parent = lua_isstring(L, 5) ? editor::Resolve(lua_tostring(L, 5)) : nullptr;
    if (!parent)
        if (engine::ZGEOM* hitman = engine::Player())
            parent = editor::RoomOf(hitman);
    if (!parent)
        return luaL_error(L, "mesh_object: no parent");
    uint32_t flags = uint32_t(luaL_optinteger(L, 6, 0x200F3)) & 0xFFFFF;  // a record's node flags only
    const float mat[9] = {0, 0, 1, 0, 1, 0, 1, 0, 0};  // rows forward, up, right
    std::string name = "Imported_" + std::to_string(++g_spawnCounter), err;
    engine::ZGEOM* g = engine::CreateObject(name, parent, "ZSTDOBJ", mesh, flags, mat, pos, {}, &err);
    if (!g)
        return luaL_error(L, "mesh_object: %s", err.c_str());
    g_scriptObjects.push_back(g);
    editor::InvalidateSceneModel();
    PushObject(L, editor::PathOf(g));
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

// obj:event(name) -> sent: a script event for the character's behaviour (as the game's
// own: "Msg_InstantDeath", "HITMAN_DEAD", "Msg_ChangeMusicToActionMode", ...)
int Event(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    static std::map<std::string, std::string> names;  // the event names stay alive (the engine may keep them)
    size_t len;
    const char* asked = luaL_checklstring(L, 2, &len);
    if (len == 0 || len > 128)
        return luaL_argerror(L, 2, "an event name of 1..128 characters");
    if (!names.count(asked) && names.size() >= 1024)
        return luaL_error(L, "event: too many different event names");
    std::string& name = names[asked];
    if (name.empty())
        name = asked;
    const char* param = name.c_str();
    lua_pushboolean(L, engine::SendMessage(g, g, engine::RegisterMessage("ScriptEvent"), &param));
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
    const auto& snap = editor::FresherSceneModel();  // children made since: at most 50 ms old
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
// npc:alive() -> bool (a knocked out character is alive)
int Alive(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    lua_pushboolean(L, engine::IsAlive(g) || editor::KnockedOut(g));
    return 1;
}

// npc:knockedout() -> bool
int KnockedOutFn(lua_State* L)
{
    lua_pushboolean(L, editor::KnockedOut(Geom(L)));
    return 1;
}

void Place(engine::ZGEOM* g, const editor::Transform& t);  // below

// npc:walkto(x, y, z) / npc:walkto(object) -> bool: walks there (its
// behaviour may send it elsewhere: a character without one keeps it)
int WalkTo(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    if (!engine::IsA(g, "ZActor"))
        return luaL_error(L, "walkto: %s is not a character", engine::SafeString(g->name).c_str());
    float pos[3];
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "_path");
        engine::ZGEOM* t = editor::Resolve(luaL_checkstring(L, -1));
        lua_pop(L, 1);
        if (!t || !editor::IsLive(t))
            return luaL_error(L, "walkto: the target is not in the loaded scene");
        engine::WorldPosition(t, pos);
    } else {
        for (int i = 0; i < 3; ++i)
            pos[i] = Coordinate(L, 2 + i);
    }
    editor::CancelSettle(g);  // its own walk finds its room after a setpos
    lua_pushboolean(L, engine::ActorWalkTo(g, pos));
    return 1;
}

// npc:lookat(object) -> bool: turns its head to the object; npc:lookat(nil) stops
int LookAt(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    engine::ZGEOM* t = nullptr;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "_path");
        t = editor::Resolve(luaL_checkstring(L, -1));
        lua_pop(L, 1);
        if (!t || !editor::IsLive(t))
            return luaL_error(L, "lookat: the target is not in the loaded scene");
    }
    if (!engine::IsA(g, "ZActor"))
        return luaL_error(L, "lookat: %s is not a character", engine::SafeString(g->name).c_str());
    lua_pushboolean(L, engine::ActorLookAt(g, t));
    return 1;
}

// npc:moveset(name) -> bool: how it moves from now on ("Walk", "Run", ...)
int MoveSet(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    size_t n;
    const char* name = luaL_checklstring(L, 2, &n);
    if (n == 0 || n > 63)
        return luaL_argerror(L, 2, "a move set name");
    for (size_t i = 0; i < n; ++i)
        if (name[i] < 0x20 || name[i] > 0x7E)
            return luaL_argerror(L, 2, "a move set name");
    if (!engine::IsA(g, "ZActor"))
        return luaL_error(L, "moveset: %s is not a character", engine::SafeString(g->name).c_str());
    lua_pushboolean(L, engine::ActorUseMoveSet(g, name));
    return 1;
}

// npc:animation(name [, loop]) -> bool: plays one of its animations
// ("Stand_Relaxed_Alt01", ...); false: it has none of that name
int Animation(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    size_t n;
    const char* name = luaL_checklstring(L, 2, &n);
    if (n == 0 || n > 127)
        return luaL_argerror(L, 2, "an animation name");
    for (size_t i = 0; i < n; ++i)
        if (name[i] < 0x20 || name[i] > 0x7E)
            return luaL_argerror(L, 2, "an animation name");
    if (!engine::IsA(g, "ZActor"))
        return luaL_error(L, "animation: %s is not a character", engine::SafeString(g->name).c_str());
    lua_pushboolean(L, engine::ActorPlayAnimation(g, name, lua_toboolean(L, 3)));
    return 1;
}

// npc:faceto(object) / npc:faceto(x, y, z): turns it (at once, as the
// scripts' FaceObject 0x58AB10) towards the point on the level
int FaceTo(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    float to[3];
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "_path");
        engine::ZGEOM* t = editor::Resolve(luaL_checkstring(L, -1));
        lua_pop(L, 1);
        if (!t || !editor::IsLive(t))
            return luaL_error(L, "faceto: the target is not in the loaded scene");
        engine::WorldPosition(t, to);
    } else {
        for (int i = 0; i < 3; ++i)
            to[i] = Coordinate(L, 2 + i);
    }
    editor::Transform tr = editor::GetTransform(g);
    float dx = to[0] - tr.pos[0], dz = to[2] - tr.pos[2];
    if (dx * dx + dz * dz < 1.0f) {
        lua_pushboolean(L, false);
        return 1;
    }
    // a character's face is opposite its node's forward row (seen in the game)
    float yaw = std::atan2(dx, dz) / 0.017453292f + (engine::IsA(g, "ZActor") ? 180.0f : 0.0f);
    editor::EulerToMatrix(yaw, 0.0f, 0.0f, tr.mat);
    Place(g, tr);
    lua_pushboolean(L, true);
    return 1;
}

// npc:kill() -> bool (killed now)
int Kill(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    if (engine::ZGEOM* a = editor::ActorOf(g))
        g = a;  // its group
    if (!engine::IsA(g, "ZActor"))
        return luaL_error(L, "kill: %s is not a character", engine::SafeString(g->name).c_str());
    if (editor::KnockedOut(g)) {  // not alive for the engine: its behaviour still takes the instant death
        static const char* param = "Msg_InstantDeath";
        lua_pushboolean(L, engine::SendMessage(g, g, engine::RegisterMessage("ScriptEvent"), &param));
        return 1;
    }
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

// the last part of the path ("Crate#2" -> "Crate"): also for an object gone meanwhile (a sound
// the game made for a moment), so a loop over h2.objects() does not stop on it
int Name(lua_State* L)
{
    std::string path = PathArg(L);
    std::string name = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
    if (size_t hash = name.rfind('#'); hash != std::string::npos && hash + 1 < name.size() &&
                                       std::isdigit(static_cast<unsigned char>(name[hash + 1])))
        name.resize(hash);
    lua_pushstring(L, name.c_str());
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

// Characters keep their own position (and write it into their node every
// frame): they are teleported.
void Place(engine::ZGEOM* g, const editor::Transform& t)
{
    editor::SetTransform(g, t);  // characters teleported, Hitman by his group
}

int SetPos(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    editor::Transform t = editor::GetTransform(g);
    for (int i = 0; i < 3; ++i)
        t.pos[i] = Coordinate(L, 2 + i);
    Place(g, t);
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
    float y = Coordinate(L, 2), p = lua_isnoneornil(L, 3) ? 0.0f : Coordinate(L, 3),
          r = lua_isnoneornil(L, 4) ? 0.0f : Coordinate(L, 4);
    editor::EulerToMatrix(y, p, r, t.mat);
    Place(g, t);
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

// obj:active([on]) -> on: in the world (the scripts' MakeActive / MakeInactive: taken out of
// its rooms, its routines stopped, its children too)
int Active(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    if (!lua_isnoneornil(L, 2) && !engine::SetGeomActive(g, lua_toboolean(L, 2) != 0))
        return luaL_error(L, "active: refused");
    lua_pushboolean(L, engine::GeomActive(g));
    return 1;
}

// obj:send(message [, number]) -> sent: a message as the levels' scripts send one
// (SendCommand / SendCommand_i: "Msg_LockDoor", "Msg_UnlockDoor", "DoorOpen"...)
int Send(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    lua_pushboolean(L, engine::SendCommand(g, luaL_checkstring(L, 2), uint32_t(luaL_optinteger(L, 3, 0))));
    return 1;
}

// npc:unconsciousbody([on]) -> on: the scripts' mark that its body is an unconscious one (SetBodyUnconscious;
// the other characters' scripts ask it when they find a body)
int UnconsciousBody(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    if (engine::BodyUnconscious(g) < 0)
        return luaL_error(L, "unconsciousbody: %s is not one of the scenes' characters", engine::SafeString(g->name).c_str());
    if (!lua_isnoneornil(L, 2))
        engine::SetBodyUnconscious(g, lua_toboolean(L, 2) != 0);
    lua_pushboolean(L, engine::BodyUnconscious(g) == 1);
    return 1;
}

// obj:sound(name) -> id | nil: one of the level's sounds played at the object (the scripts' Play3dSound)
int ObjSound(lua_State* L)
{
    uint32_t ref = engine::PlaySound3D(luaL_checkstring(L, 2), Geom(L));
    if (!ref)
        return 0;
    lua_pushinteger(L, lua_Integer(ref));
    return 1;
}

// npc:draggable([on]) -> on: Hitman can drag its body (the scripts' EnableDrag / DisableDrag)
int DraggableFn(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    if (engine::Draggable(g) < 0)
        return luaL_error(L, "draggable: %s is not one of the scenes' characters", engine::SafeString(g->name).c_str());
    if (!lua_isnoneornil(L, 2) && !engine::SetDraggable(g, lua_toboolean(L, 2) != 0))
        return luaL_error(L, "draggable: refused");
    lua_pushboolean(L, engine::Draggable(g) == 1);
    return 1;
}

// npc:noticed([on]) -> on: the other characters take notice of it (the scripts'
// MakeActorVisible / MakeActorInvisible: the scene's TrackLinkObjects)
int NoticedFn(lua_State* L)
{
    engine::ZGEOM* g = Geom(L);
    if (engine::Noticed(g) < 0)
        return luaL_error(L, "noticed: %s is not one of the scenes' characters", engine::SafeString(g->name).c_str());
    if (!lua_isnoneornil(L, 2) && !engine::SetNoticed(g, lua_toboolean(L, 2) != 0))
        return luaL_error(L, "noticed: refused");
    lua_pushboolean(L, engine::Noticed(g) == 1);
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
        {"spawn", ObjSpawn}, {"destroy", Destroy}, {"spawned", Spawned}, {"behaviour", Behaviour}, {"state", State}, {"event", Event},
        {"children", Children}, {"parent", Parent}, {"flags", Flags}, {"mesh", Mesh}, {"addr", Addr}, {"select", SelectObj},
        {"points", Points}, {"setpoints", SetPoints}, {"alive", Alive}, {"knockedout", KnockedOutFn}, {"kill", Kill},
        {"walkto", WalkTo}, {"lookat", LookAt}, {"moveset", MoveSet}, {"animation", Animation}, {"faceto", FaceTo},
        {"active", Active}, {"draggable", DraggableFn}, {"noticed", NoticedFn}, {"send", Send},
        {"unconsciousbody", UnconsciousBody}, {"sound", ObjSound},
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
    Register("import_models", ImportModels);
    Register("imported_mesh", ImportedMesh);
    Register("mesh_object", MeshObject);
    Register("item", Item);
    Register("npcs", Npcs);
    Register("behaviours", Behaviours);
    Register("npc", Npc);
    Register("point", Point);
    Register("player", Player);
    Register("objective", Objective);
    Register("briefing", Briefing);
    Register("sky", Sky);
    Register("mission", Mission);
    Register("_subtitle", Subtitle);
    Register("info", Info);
    Register("basemission", BaseMission);
    Register("onpath", OnPath);
    Register("walkable", Walkable);
    Register("disguise", Disguise);
    Register("give", Give);
    Register("health", Health);
    Register("capacity", Capacity);
    Register("music", Music);
    // h2.outfit(model): Hitman in the clothes of a character model of the map (as a level's
    // start_outfit): one of it is made under the map, gives its clothes and stays out of sight
    if (luaL_dostring(L, R"lua(
      h2.outfit = function(model)
        assert(type(model) == "string", "outfit: a character model name (h2.npcs() lists them)")
        local x, y, z = h2.player():pos()
        local n = h2.npc(model, x, y - 3000, z, 0, false)
        h2.disguise(n)
        n:hide()
        return n
      end)lua") != LUA_OK)
        lua_pop(L, 1);
}

void DestroyScriptObjects()
{
    for (engine::ZGEOM* g : g_scriptObjects)
        if (editor::IsSpawned(g))
            editor::DestroySpawned(g);
    g_scriptObjects.clear();
}

void ForgetScriptObjects() { g_scriptObjects.clear(); }

void PushObjectPath(lua_State* L, const std::string& path) { PushObject(L, path); }

namespace {

// geom -> snapshot index of the scene model, rebuilt with it (events can carry many objects)
const engine::SceneSnapshot* g_indexOf;
const engine::GeomInfo* g_indexData;
size_t g_indexSize;
std::unordered_map<const engine::ZGEOM*, int> g_index;
double g_refreshed;

bool LooksLikeGeom(engine::ZGEOM* g)
{
    __try {
        return g->node && g->node->geom == g;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int IndexOf(engine::ZGEOM* g)
{
    const engine::SceneSnapshot* snap = &editor::SceneModel();
    for (int pass = 0; pass < 2; ++pass) {
        if (snap != g_indexOf || snap->geoms.data() != g_indexData || snap->geoms.size() != g_indexSize) {
            g_index.clear();
            for (int i = 0; i < int(snap->geoms.size()); ++i)
                g_index.emplace(snap->geoms[i].geom, i);
            g_indexOf = snap;
            g_indexData = snap->geoms.data();
            g_indexSize = snap->geoms.size();
        }
        if (auto it = g_index.find(g); it != g_index.end())
            return it->second;
        // made since the snapshot: a new one (at most every 100 ms)
        if (pass || NowMs() - g_refreshed < 100.0 || !LooksLikeGeom(g))
            break;
        g_refreshed = NowMs();
        snap = &editor::SceneModel(0);
    }
    return -1;
}

} // namespace

bool PushGeom(lua_State* L, engine::ZGEOM* g)
{
    int i = g && reinterpret_cast<uintptr_t>(g) >= 0x10000 ? IndexOf(g) : -1;
    if (i < 0) {
        lua_pushnil(L);
        return false;
    }
    PushObject(L, editor::SceneModel().geoms[i].path);
    return true;
}

bool PushScriptGeom(lua_State* L, void* instance)
{
    return PushGeom(L, instance ? engine::ScriptGeom(instance, editor::SceneModel()) : nullptr);
}

bool PushCarriedGeom(lua_State* L, uint32_t value)
{
    if (value >= 0x10000) {
        if (PushGeom(L, reinterpret_cast<engine::ZGEOM*>(uintptr_t(value))))
            return true;
        lua_pop(L, 1);
    }
    // a geom reference: dynamic ones are looked up in a table, static ones are
    // offsets of 0x70 byte nodes
    bool ref = value && !(value & 0x80000000) && (!(value & 0x40000000) || (value & 0x3FFFFFFF) % 0x70 == 0);
    return PushGeom(L, ref ? engine::GeomOfRef(value) : nullptr);
}

} // namespace h2::script
