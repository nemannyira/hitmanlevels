#include "events.h"

#include <cctype>
#include <iterator>
#include <map>
#include <set>

#include "../engine/game_events.h"
#include "../log.h"
#include "objects_api.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "lua_vm.h"

namespace h2::script {

namespace {

// registry: name -> { {fn = f, owner = "level"}, ... }
constexpr const char* kHandlers = "h2.handlers";
std::string g_owner;

std::string Lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The game's messages under names of their own (the same table as
// "msg:<name>", with a field: `flag` 1 / 0 sets it to true / false, -1 to
// whether the message's param is not 0). `once`:
//   Shared: a message many objects get (a noise everyone near hears, over
//           half a second: its words are where it was): once a second;
//   Each:   a state a character gets again and again (a guard's threads:
//           Thread_Recognized many times): once a second per character.
// A C1-1 guard (seen in the game): Thread_ExamineHitman / ActingStrange
// (suspicious), Thread_Recognized, Thread_Angry, ReportNewTarget (takes
// Hitman as his target), HelpRequest (calls the others).
enum Once { Always, Shared, Each };
struct Alias {
    const char* event;
    const char* message;  // lower case
    const char* field;
    int flag;
    Once once;
};
const Alias kAliases[] = {
    {"suspicious", "thread_examinehitman", nullptr, 0, Each},
    {"suspicious", "thread_actingstrange", nullptr, 0, Each},
    {"recognized", "thread_recognized", nullptr, 0, Each},
    {"hostile", "thread_angry", nullptr, 0, Each},
    {"spotted", "hitmanspotted", nullptr, 0, Each},
    {"spotted", "hitmandetected", nullptr, 0, Each},
    {"spotted", "reportnewtarget", nullptr, 0, Each},
    {"help", "helprequest", nullptr, 0, Each},
    {"alarm", "enteralarmstate", "on", 1, Each},  // levels whose scripts have an alarm state
    {"alarm", "alarmraised", "on", 1, Always},
    {"alarm", "leavealarmstate", "on", 0, Each},
    {"alarm", "alarmend", "on", 0, Always},
    {"unconscious", "characterunconscious", nullptr, 0, Always},
    {"disguise", "levelcontrolnotifychangeclothes", nullptr, 0, Shared},  // (Hitman_ChangedClothes comes with it)
    {"pickup", "levelcontrolnotifypickupitem", nullptr, 0, Always},
    {"useitem", "levelcontrolnotifyuseitem", nullptr, 0, Always},
    {"drop", "msg_dropitem", nullptr, 0, Always},
    {"damage", "msg_damage", nullptr, 0, Always},
    {"hit", "projectilehit", nullptr, 0, Always},
    {"shot", "msg_sound_shot", "silent", 0, Shared},
    {"shot", "msg_sound_silentshot", "silent", 1, Shared},
    {"door", "dooropen", "open", 1, Always},
    {"door", "doorclose", "open", 0, Always},
    {"explosion", "msg_explode", nullptr, 0, Shared},
    {"laptop", "briefingmenuopened", "open", -1, Shared},
    {"map", "ingamemapopened", "open", -1, Shared},
    {"menu", "ingamemenuopened", "open", -1, Shared},
    {"inventory", "ingameinventoryopened", "open", -1, Shared},
    {"cutscene", "cutsequence_start", "started", 1, Shared},
    {"cutscene", "cutsequence_end", "started", 0, Shared},
    {"restart", "restartlevel", nullptr, 0, Shared},
};

// "msg:HitmanSpotted" and "msg:hitmanspotted" are one event
std::string EventKey(const char* name)
{
    std::string n = name;
    if (n.size() > 4 && Lower(n.substr(0, 4)) == "msg:")
        return Lower(n);
    return n;
}

// the messages the handlers need
void UpdateWatch()
{
    lua_State* L = State();
    std::set<std::string> names;
    bool all = false;
    lua_getfield(L, LUA_REGISTRYINDEX, kHandlers);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        if (lua_type(L, -2) == LUA_TSTRING && lua_istable(L, -1) && lua_rawlen(L, -1) > 0) {
            std::string key = lua_tostring(L, -2);
            if (key == "message")
                all = true;
            else if (key.rfind("msg:", 0) == 0)
                names.insert(key.substr(4));
            for (const Alias& a : kAliases)
                if (key == a.event)
                    names.insert(a.message);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    engine::WatchMessages(names, all);
}

// h2.on(name, fn)
int On(lua_State* L)
{
    std::string name = EventKey(luaL_checkstring(L, 1));
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_getfield(L, LUA_REGISTRYINDEX, kHandlers);
    if (lua_getfield(L, -1, name.c_str()) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, name.c_str());
    }
    lua_createtable(L, 0, 2);
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, "fn");
    lua_pushstring(L, g_owner.c_str());
    lua_setfield(L, -2, "owner");
    lua_rawseti(L, -2, lua_rawlen(L, -2) + 1);
    lua_pop(L, 2);
    UpdateWatch();
    return 0;
}

// h2.off(name): removes every handler of the event
int Off(lua_State* L)
{
    std::string name = EventKey(luaL_checkstring(L, 1));
    lua_getfield(L, LUA_REGISTRYINDEX, kHandlers);
    lua_pushnil(L);
    lua_setfield(L, -2, name.c_str());
    lua_pop(L, 1);
    UpdateWatch();
    return 0;
}

// h2.messages(): every message name the engine knows now (lower case)
int Messages(lua_State* L)
{
    std::vector<std::string> names = engine::MessageNames();
    lua_createtable(L, int(names.size()), 0);
    for (size_t i = 0; i < names.size(); ++i) {
        lua_pushstring(L, names[i].c_str());
        lua_rawseti(L, -2, lua_Integer(i + 1));
    }
    return 1;
}

// Calls the handlers of an event. Everything runs in a protected call: a
// Lua error while the arguments are made (out of memory) must not stop the
// game (an unprotected error aborts).
template <typename Push>
void Fire(const std::string& name, Push push)
{
    lua_State* L = State();
    if (!L)
        return;
    struct Call {
        const std::string* name;
        Push* push;
    } call{&name, &push};
    lua_pushcfunction(L, [](lua_State* L) -> int {
        const Call& c = *static_cast<const Call*>(lua_touserdata(L, 1));
        lua_getfield(L, LUA_REGISTRYINDEX, kHandlers);
        if (lua_getfield(L, -1, c.name->c_str()) != LUA_TTABLE)
            return 0;
        lua_Integer n = lua_rawlen(L, -1);
        for (lua_Integer i = 1; i <= n; ++i) {
            int top = lua_gettop(L);
            lua_rawgeti(L, -1, i);
            lua_getfield(L, -1, "fn");
            lua_remove(L, -2);
            int nargs = (*c.push)(L);
            if (PCall(L, nargs, 0, 0) != LUA_OK) {
                const char* err = lua_tostring(L, -1);
                ConsolePrint(LineKind::Error, *c.name + " handler: " + (err ? err : "error"));
            }
            lua_settop(L, top);
        }
        return 0;
    });
    lua_pushlightuserdata(L, &call);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        ConsolePrint(LineKind::Error, name + " event: " + (err ? err : "error"));
        lua_pop(L, 1);
    }
}

bool HasHandlers(const std::string& name)
{
    lua_State* L = State();
    lua_getfield(L, LUA_REGISTRYINDEX, kHandlers);
    bool has = lua_getfield(L, -1, name.c_str()) == LUA_TTABLE && lua_rawlen(L, -1) > 0;
    lua_pop(L, 2);
    return has;
}

void SetInteger(lua_State* L, const char* field, lua_Integer v)
{
    lua_pushinteger(L, v);
    lua_setfield(L, -2, field);
}

void SetBool(lua_State* L, const char* field, bool v)
{
    lua_pushboolean(L, v);
    lua_setfield(L, -2, field);
}

// {name, to, from, object, value, words, param, text}
void PushMessage(lua_State* L, const engine::GameEvent& e)
{
    lua_createtable(L, 0, 8);
    lua_pushstring(L, e.name.c_str());
    lua_setfield(L, -2, "name");
    if (e.script)
        PushScriptGeom(L, e.instance);
    else
        PushGeom(L, e.to);
    lua_setfield(L, -2, "to");
    PushGeom(L, e.from);
    lua_setfield(L, -2, "from");
    // the object the message is about: the param itself (a pointer or a
    // geom ref) or what it points to
    if (!PushCarriedGeom(L, uint32_t(e.param))) {
        lua_pop(L, 1);
        if (e.hasValue)
            PushCarriedGeom(L, e.value);
        else
            lua_pushnil(L);
    }
    lua_setfield(L, -2, "object");
    if (e.hasValue) {
        SetInteger(L, "value", lua_Integer(int32_t(e.value)));
        lua_createtable(L, e.wordCount, 0);
        for (int i = 0; i < e.wordCount; ++i) {
            lua_pushinteger(L, lua_Integer(int32_t(e.words[i])));
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, -2, "words");
    }
    SetInteger(L, "param", lua_Integer(e.param));
    if (!e.text.empty()) {
        lua_pushstring(L, e.text.c_str());
        lua_setfield(L, -2, "text");
    }
}

const char* KindName(int type)
{
    // the statistics count type 2 as enemies, 1 as innocents (0x5058B0)
    return type == 2 ? "enemy" : type == 1 ? "innocent" : "other";
}

void FireVictim(const char* event, const engine::GameEvent& e)
{
    Fire(event, [&](lua_State* L) {
        lua_createtable(L, 0, 3);
        PushCarriedGeom(L, e.value);
        lua_setfield(L, -2, "victim");
        lua_pushstring(L, KindName(e.type));
        lua_setfield(L, -2, "kind");
        SetInteger(L, "type", e.type);
        return 1;
    });
}

std::map<std::string, double> g_recent;  // `once` events fired lately

void FireGameMessage(const engine::GameEvent& e)
{
    std::string name = Lower(e.name);
    auto push = [&](lua_State* L) {
        PushMessage(L, e);
        return 1;
    };
    Fire("message", push);
    Fire("msg:" + name, push);
    for (const Alias& a : kAliases) {
        if (name != a.message || !HasHandlers(a.event))
            continue;
        bool flag = a.flag < 0 ? e.param != 0 : a.flag != 0;
        if (a.once != Always) {
            std::string key = std::string(a.event) + (flag ? "1" : "0");
            if (a.once == Each)
                key += ":" + std::to_string(e.script ? uintptr_t(e.instance) : uintptr_t(e.to));
            else
                for (int i = 0; i < e.wordCount && i < 3; ++i)
                    key += ":" + std::to_string(e.words[i]);
            double now = NowMs();
            auto it = g_recent.find(key);
            if (it != g_recent.end() && now - it->second < 1000.0)
                continue;
            g_recent[key] = now;
        }
        Fire(a.event, [&](lua_State* L) {
            PushMessage(L, e);
            if (a.field)
                SetBool(L, a.field, flag);
            return 1;
        });
    }
}

} // namespace

void FireGameEvents()
{
    std::vector<engine::GameEvent> events = engine::TakeGameEvents();
    if (g_recent.size() > 256) {
        double now = NowMs();
        for (auto it = g_recent.begin(); it != g_recent.end();)
            it = now - it->second > 2000.0 ? g_recent.erase(it) : std::next(it);
    }
    for (const engine::GameEvent& e : events) {
        switch (e.kind) {
        case engine::GameEvent::Message:
            FireGameMessage(e);
            break;
        case engine::GameEvent::Kill:
            FireVictim("kill", e);
            break;
        case engine::GameEvent::Harm:
            FireVictim("harm", e);
            break;
        case engine::GameEvent::MissionCompleted:
        case engine::GameEvent::MissionFailed:
            Fire("mission", [&](lua_State* L) {
                lua_createtable(L, 0, 1);
                lua_pushstring(L, e.kind == engine::GameEvent::MissionCompleted ? "completed" : "failed");
                lua_setfield(L, -2, "result");
                return 1;
            });
            break;
        case engine::GameEvent::Drag:
            Fire("drag", [&](lua_State* L) {
                lua_createtable(L, 0, 3);
                PushGeom(L, e.to);
                lua_setfield(L, -2, "body");
                if (e.hasValue && e.value)
                    PushCarriedGeom(L, e.value);
                else
                    lua_pushnil(L);
                lua_setfield(L, -2, "by");
                SetBool(L, "started", e.type != 0);
                return 1;
            });
            break;
        case engine::GameEvent::HitmanDamage:
            Fire("hitman_damage", [&](lua_State* L) {
                lua_createtable(L, 0, 3);
                lua_pushnumber(L, e.amount);
                lua_setfield(L, -2, "amount");
                lua_pushnumber(L, e.health);
                lua_setfield(L, -2, "health");
                SetBool(L, "dead", e.health <= 0.0f);
                return 1;
            });
            break;
        }
    }
}

void RegisterEvents()
{
    lua_State* L = State();
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kHandlers);
    Register("on", On);
    Register("off", Off);
    Register("messages", Messages);
}

void SetHandlerOwner(const std::string& owner) { g_owner = owner; }

void ClearHandlers(const std::string& owner)
{
    lua_State* L = State();
    lua_getfield(L, LUA_REGISTRYINDEX, kHandlers);
    lua_pushnil(L);
    while (lua_next(L, -2)) {  // key = event name, value = handler list
        lua_newtable(L);       // filtered list
        lua_Integer n = lua_rawlen(L, -2), k = 0;
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, -2, i);
            lua_getfield(L, -1, "owner");
            bool drop = owner == (lua_tostring(L, -1) ? lua_tostring(L, -1) : "");
            lua_pop(L, 1);
            if (drop)
                lua_pop(L, 1);
            else
                lua_rawseti(L, -2, ++k);
        }
        lua_pushvalue(L, -3);   // key
        lua_insert(L, -2);      // key, filtered
        lua_rawset(L, -5);      // handlers[key] = filtered (same key: allowed during traversal)
        lua_pop(L, 1);          // old list
    }
    lua_pop(L, 1);
    UpdateWatch();
}

void FireScene(const std::string& scene)
{
    Fire("scene", [&](lua_State* L) {
        lua_pushstring(L, scene.c_str());
        return 1;
    });
}

void FireDeath(const std::string& path)
{
    Fire("death", [&](lua_State* L) {
        PushObjectPath(L, path);
        return 1;
    });
}

void FireObjective(int slot)
{
    Fire("objective", [&](lua_State* L) {
        lua_pushinteger(L, slot);
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
