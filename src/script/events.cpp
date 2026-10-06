#include "events.h"

#include <cctype>
#include <set>

#include "../engine/game_events.h"
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
// whether the message's param is not 0). `once`: a message many objects get
// (a noise, a change of clothes): the event comes once a frame.
struct Alias {
    const char* event;
    const char* message;  // lower case
    const char* field;
    int flag;
    bool once;
};
const Alias kAliases[] = {
    {"spotted", "hitmanspotted", nullptr, 0, false},
    {"spotted", "hitmandetected", nullptr, 0, false},
    {"alarm", "enteralarmstate", "on", 1, false},
    {"alarm", "alarmraised", "on", 1, false},
    {"alarm", "leavealarmstate", "on", 0, false},
    {"alarm", "alarmend", "on", 0, false},
    {"unconscious", "characterunconscious", nullptr, 0, false},
    {"disguise", "hitman_changedclothes", nullptr, 0, true},
    {"disguise", "levelcontrolnotifychangeclothes", nullptr, 0, true},
    {"pickup", "levelcontrolnotifypickupitem", nullptr, 0, false},
    {"useitem", "levelcontrolnotifyuseitem", nullptr, 0, false},
    {"drop", "msg_dropitem", nullptr, 0, false},
    {"damage", "msg_damage", nullptr, 0, false},
    {"hit", "projectilehit", nullptr, 0, false},
    {"shot", "msg_sound_shot", "silent", 0, true},
    {"shot", "msg_sound_silentshot", "silent", 1, true},
    {"door", "dooropen", "open", 1, false},
    {"door", "doorclose", "open", 0, false},
    {"explosion", "msg_explode", nullptr, 0, true},
    {"drag", "msg_dragstart", "started", 1, false},
    {"drag", "msg_dragend", "started", 0, false},
    {"laptop", "briefingmenuopened", "open", -1, true},
    {"map", "ingamemapopened", "open", -1, true},
    {"menu", "ingamemenuopened", "open", -1, true},
    {"inventory", "ingameinventoryopened", "open", -1, true},
    {"cutscene", "cutsequence_start", "started", 1, true},
    {"cutscene", "cutsequence_end", "started", 0, true},
    {"restart", "restartlevel", nullptr, 0, true},
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

template <typename Push>
void Fire(const std::string& name, Push push)
{
    lua_State* L = State();
    if (!L)
        return;
    lua_getfield(L, LUA_REGISTRYINDEX, kHandlers);
    if (lua_getfield(L, -1, name.c_str()) != LUA_TTABLE) {
        lua_pop(L, 2);
        return;
    }
    lua_Integer n = lua_rawlen(L, -1);
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, i);
        lua_getfield(L, -1, "fn");
        lua_remove(L, -2);
        int nargs = push(L);
        if (PCall(L, nargs, 0, 0) != LUA_OK) {
            const char* err = lua_tostring(L, -1);
            ConsolePrint(LineKind::Error, name + " handler: " + (err ? err : "error"));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 2);
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

// {name, to, from, object, value, param, text}
void PushMessage(lua_State* L, const engine::GameEvent& e)
{
    lua_createtable(L, 0, 8);
    lua_pushstring(L, e.name.c_str());
    lua_setfield(L, -2, "name");
    PushGeom(L, e.to);
    lua_setfield(L, -2, "to");
    PushGeom(L, e.from);
    lua_setfield(L, -2, "from");
    // the object the message is about: the param itself or what it points to
    if (!PushGeom(L, reinterpret_cast<engine::ZGEOM*>(e.param))) {
        lua_pop(L, 1);
        if (e.hasValue)
            PushCarriedGeom(L, e.value);
        else
            lua_pushnil(L);
    }
    lua_setfield(L, -2, "object");
    if (e.hasValue)
        SetInteger(L, "value", lua_Integer(int32_t(e.value)));
    SetInteger(L, "param", lua_Integer(e.param));
    if (!e.text.empty()) {
        lua_pushstring(L, e.text.c_str());
        lua_setfield(L, -2, "text");
    }
}

const char* KindName(int type)
{
    return type == 1 ? "enemy" : type == 2 ? "innocent" : "other";
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

void FireGameMessage(const engine::GameEvent& e, std::set<std::string>& once)
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
        if (a.once && !once.insert(std::string(a.event) + (flag ? "1" : "0")).second)
            continue;
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
    std::set<std::string> once;
    for (const engine::GameEvent& e : events) {
        switch (e.kind) {
        case engine::GameEvent::Message:
            FireGameMessage(e, once);
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
