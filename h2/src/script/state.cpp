#include "state.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "json.hpp"

#include "../engine/imports.h"
#include "../game.h"
#include "../hook.h"
#include "../log.h"
#include "../scene.h"
#include "events.h"
#include "lua_vm.h"

namespace h2::script {

namespace {

constexpr uintptr_t kSaveGame = 0x41AAD0;        // ZEngineDataBase::SaveGame(slot) -> bool
constexpr uintptr_t kLoadGame2 = 0x41B1C0;       // ZEngineDataBase::LoadGame2(): the objects' state
constexpr uintptr_t kDeleteSaveGame = 0x41AE90;  // ZEngineDataBase::DeleteSaveGame(slot) -> bool
constexpr size_t kSys_LoadingSlot = 0x18A8;      // the slot LoadGame1 / 2 read
constexpr int kMaxDepth = 32;
constexpr size_t kMaxValues = 100000;
constexpr size_t kMaxFile = 4 << 20;
constexpr int kLoadEventFrames = 120;            // without level scripts: the "load" event after this

using json = nlohmann::json;

void (*g_beforeGameLoad)();
std::string (*g_saveExtra)();
void (*g_loadExtra)(const std::string&);
constexpr const char* kExtraKey = "__editor";
int g_loadedSlot = -1;   // a "load" event to fire (-1: none)
int g_loadFrames;

// a downloaded level's are its own (its id: [a-z0-9]): it reads no other level's state
std::string StatePath(unsigned slot)
{
    const std::string& tag = UntrustedTag();
    return GameDir() + "Save\\" + std::to_string(slot) + (tag.empty() ? "" : "." + tag) + ".h2lua";
}

bool SlotSaved(unsigned slot)
{
    std::string p = GameDir() + "Save\\" + std::to_string(slot) + ".1";
    return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// ---- Lua <-> JSON ----

bool ToJson(lua_State* L, int idx, int depth, size_t& count, json& out, std::string& warn);

bool TableToJson(lua_State* L, int idx, int depth, size_t& count, json& out, std::string& warn)
{
    idx = lua_absindex(L, idx);
    lua_Integer n = lua_Integer(lua_rawlen(L, idx));
    bool list = n > 0;
    size_t keys = 0;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        ++keys;
        if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1 || lua_tointeger(L, -2) > n)
            list = false;
        lua_pop(L, 1);
    }
    list = list && keys == size_t(n);
    out = list ? json::array() : json::object();
    if (list) {
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, idx, i);
            json v;
            if (!ToJson(L, -1, depth + 1, count, v, warn)) {
                lua_pop(L, 1);
                return false;
            }
            out.push_back(std::move(v));
            lua_pop(L, 1);
        }
        return true;
    }
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        std::string key;
        if (lua_type(L, -2) == LUA_TSTRING) {
            size_t len;
            const char* s = lua_tolstring(L, -2, &len);
            key.assign(s, len);
        } else if (lua_type(L, -2) == LUA_TNUMBER) {
            lua_pushvalue(L, -2);  // tostring on a copy: lua_next needs the key as it is
            key = lua_tostring(L, -1);
            lua_pop(L, 1);
        } else {
            warn = "a key that is not a string or a number";
            lua_pop(L, 1);
            continue;
        }
        json v;
        if (!ToJson(L, -1, depth + 1, count, v, warn)) {
            lua_pop(L, 2);
            return false;
        }
        if (!v.is_null())
            out[key] = std::move(v);
        lua_pop(L, 1);
    }
    return true;
}

bool ToJson(lua_State* L, int idx, int depth, size_t& count, json& out, std::string& warn)
{
    if (depth > kMaxDepth || ++count > kMaxValues) {
        warn = "too deep or too large (a table in itself?)";
        return false;
    }
    switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN:
        out = lua_toboolean(L, idx) != 0;
        return true;
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx)) {
            out = int64_t(lua_tointeger(L, idx));
        } else {
            double d = lua_tonumber(L, idx);
            if (!std::isfinite(d)) {
                warn = "a number that is not finite";
                out = nullptr;
            } else {
                out = d;
            }
        }
        return true;
    case LUA_TSTRING: {
        size_t len;
        const char* s = lua_tolstring(L, idx, &len);
        out = std::string(s, len);
        return true;
    }
    case LUA_TTABLE:
        return TableToJson(L, idx, depth, count, out, warn);
    case LUA_TNIL:
        out = nullptr;
        return true;
    default:
        warn = std::string("a ") + lua_typename(L, lua_type(L, idx)) + " (not kept)";
        out = nullptr;
        return true;
    }
}

void PushJson(lua_State* L, const json& v, int depth)
{
    if (depth > kMaxDepth) {
        lua_pushnil(L);
        return;
    }
    switch (v.type()) {
    case json::value_t::boolean:
        lua_pushboolean(L, v.get<bool>());
        break;
    case json::value_t::number_integer:
    case json::value_t::number_unsigned:
        lua_pushinteger(L, lua_Integer(v.get<int64_t>()));
        break;
    case json::value_t::number_float:
        lua_pushnumber(L, v.get<double>());
        break;
    case json::value_t::string: {
        const std::string& s = v.get_ref<const std::string&>();
        lua_pushlstring(L, s.data(), s.size());
        break;
    }
    case json::value_t::array: {
        lua_createtable(L, int(v.size()), 0);
        lua_Integer i = 0;
        for (const json& e : v) {
            PushJson(L, e, depth + 1);
            lua_rawseti(L, -2, ++i);
        }
        break;
    }
    case json::value_t::object:
        lua_createtable(L, 0, int(v.size()));
        for (auto it = v.begin(); it != v.end(); ++it) {
            PushJson(L, it.value(), depth + 1);
            lua_setfield(L, -2, it.key().c_str());
        }
        break;
    default:
        lua_pushnil(L);
    }
}

// how deep brackets go (outside strings): a file from elsewhere must not
// nest deep enough to exhaust the stack while parsed or freed
int NestingDepth(const std::string& text)
{
    int depth = 0, most = 0;
    bool inString = false, escaped = false;
    for (char c : text) {
        if (inString) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"')
            inString = true;
        else if (c == '[' || c == '{')
            most = std::max(most, ++depth);
        else if (c == ']' || c == '}')
            --depth;
    }
    return most;
}

// ---- h2.state ----

void ResetState()
{
    lua_State* L = State();
    if (!L)
        return;
    lua_getglobal(L, "h2");
    if (lua_istable(L, -1)) {
        lua_newtable(L);
        lua_setfield(L, -2, "state");
    }
    lua_pop(L, 1);
}

// h2.state -> JSON text ("" and an error: not written)
int SerializeState(lua_State* L)
{
    std::string* out = static_cast<std::string*>(lua_touserdata(L, 1));
    lua_getglobal(L, "h2");
    if (!lua_istable(L, -1))
        return 0;
    lua_getfield(L, -1, "state");
    json j;
    size_t count = 0;
    std::string warn;
    if (lua_istable(L, -1) && !ToJson(L, -1, 0, count, j, warn))
        return luaL_error(L, "h2.state not saved: %s", warn.c_str());
    if (!warn.empty())
        ConsolePrint(LineKind::Error, "h2.state: " + warn);
    *out = j.is_null() ? "{}" : j.dump(1, '\t', false, json::error_handler_t::replace);
    return 0;
}

int RestoreState(lua_State* L)
{
    const json* j = static_cast<const json*>(lua_touserdata(L, 1));
    lua_getglobal(L, "h2");
    if (!lua_istable(L, -1))
        return 0;
    if (j->is_object())
        PushJson(L, *j, 0);
    else
        lua_newtable(L);
    lua_setfield(L, -2, "state");
    return 0;
}

void WriteState(unsigned slot)
{
    lua_State* L = State();
    if (!L)
        return;
    std::string text;
    lua_pushcfunction(L, SerializeState);
    lua_pushlightuserdata(L, &text);
    if (PCall(L, 1, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        ConsolePrint(LineKind::Error, err ? err : "h2.state not saved");
        lua_pop(L, 1);
        return;
    }
    if (std::string extra = g_saveExtra ? g_saveExtra() : ""; !extra.empty()) {
        json all = json::parse(text, nullptr, false);
        json mine = json::parse(extra, nullptr, false);
        if (!all.is_object())
            all = json::object();
        if (!mine.is_discarded()) {
            all[kExtraKey] = mine;
            text = all.dump(1, '	', false, json::error_handler_t::replace);
        }
    }
    std::string path = StatePath(slot);
    if (text.empty() || text == "{}") {
        DeleteFileA(path.c_str());  // nothing to keep: no file (a slot saved over)
        return;
    }
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << text;
        if (!f) {
            Log("state: cannot write %s", tmp.c_str());
            return;
        }
    }
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        Log("state: cannot write %s", path.c_str());
    else
        Log("state: slot %u saved (%zu bytes)", slot, text.size());
}

void ReadState(unsigned slot)
{
    lua_State* L = State();
    if (!L)
        return;
    std::string path = StatePath(slot);
    json j = json::object();
    std::ifstream f(path, std::ios::binary);
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        std::string text = ss.str();
        if (text.size() > kMaxFile || NestingDepth(text) > kMaxDepth) {
            ConsolePrint(LineKind::Error, "h2.state of the saved game not used: too large or too deep (" + path + ")");
        } else {
            try {
                j = json::parse(text);
            } catch (const std::exception& ex) {
                ConsolePrint(LineKind::Error, std::string("h2.state of the saved game not used: ") + ex.what());
                j = json::object();
            }
        }
    }
    std::string extra;
    if (j.is_object() && j.contains(kExtraKey)) {
        extra = j[kExtraKey].dump();
        j.erase(kExtraKey);
    }
    if (g_loadExtra)
        g_loadExtra(extra);
    lua_pushcfunction(L, RestoreState);
    lua_pushlightuserdata(L, &j);
    if (PCall(L, 1, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        ConsolePrint(LineKind::Error, std::string("h2.state: ") + (err ? err : "not restored"));
        lua_pop(L, 1);
    }
    Log("state: slot %u loaded%s", slot, f ? "" : " (no state)");
}

// ---- hooks ----

using SaveGame_t = bool(__fastcall*)(void* engine, void* edx, unsigned slot);
SaveGame_t oSaveGame;
bool __fastcall hkSaveGame(void* engine, void* edx, unsigned slot)
{
    FireSlotEvent("save", int(slot));  // scripts put what they need into h2.state
    engine::SaveAuditTake();
    bool ok = oSaveGame(engine, edx, slot);
    engine::SaveAudit a = engine::SaveAuditTake();
    Log("save audit: slot %u: pointers null %d, static %d (%d renamed), anim %d, resource %d, unplaced %d%s", slot,
        a.null, a.stat, a.renamed, a.anim, a.resource, a.unplaced,
        a.unplaced ? " - THE GAME CANNOT LOAD THIS SAVE" : "");
    if (SlotSaved(slot))
        WriteState(slot);
    else
        Log("state: slot %u: no Save\\%u.1 after saving, h2.state not written", slot, slot);
    return ok;
}

using LoadGame2_t = void(__fastcall*)(void* engine, void* edx);
LoadGame2_t oLoadGame2;
void __fastcall hkLoadGame2(void* engine, void* edx)
{
    unsigned slot = 0;
    if (auto sys = reinterpret_cast<char*>(game::SysInterface()))
        slot = *reinterpret_cast<unsigned*>(sys + kSys_LoadingSlot);
    if (g_beforeGameLoad)
        g_beforeGameLoad();
    oLoadGame2(engine, edx);
    if (slot) {
        ReadState(slot);
        g_loadedSlot = int(slot);
        g_loadFrames = 0;
    }
}

using DeleteSaveGame_t = bool(__fastcall*)(void* engine, void* edx, unsigned slot);
DeleteSaveGame_t oDeleteSaveGame;
bool __fastcall hkDeleteSaveGame(void* engine, void* edx, unsigned slot)
{
    bool ok = oDeleteSaveGame(engine, edx, slot);
    DeleteFileA(StatePath(slot).c_str());
    return ok;
}

void FireLoad()
{
    int slot = g_loadedSlot;
    g_loadedSlot = -1;
    FireSlotEvent("load", slot);
}

} // namespace

void InstallState()
{
    ResetState();
    Hook("ZEngineDataBase::SaveGame", kSaveGame, hkSaveGame, &oSaveGame);
    Hook("ZEngineDataBase::LoadGame2", kLoadGame2, hkLoadGame2, &oLoadGame2);
    Hook("ZEngineDataBase::DeleteSaveGame", kDeleteSaveGame, hkDeleteSaveGame, &oDeleteSaveGame);
    // a map left: its state goes (a saved game's comes back in LoadGame2)
    OnSceneLeaving([](const std::string&) {
        ResetState();
        g_loadedSlot = -1;
    });
}

void SetBeforeGameLoad(void (*fn)()) { g_beforeGameLoad = fn; }

void SetSaveExtra(std::string (*save)(), void (*load)(const std::string&))
{
    g_saveExtra = save;
    g_loadExtra = load;
}

void UpdateState()
{
    if (g_loadedSlot >= 0 && ++g_loadFrames > kLoadEventFrames)
        FireLoad();
}

void OnLevelScriptsRan()
{
    if (g_loadedSlot >= 0)
        FireLoad();
}

} // namespace h2::script
