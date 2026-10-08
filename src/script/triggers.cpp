#include "triggers.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "../editor/playtest.h"
#include "../editor/testtools.h"
#include "../engine/scene_data.h"
#include "../log.h"
#include "../overlay/overlay.h"
#include "../scene.h"
#include "events.h"
#include "lua_vm.h"

namespace h2::script {

namespace {

constexpr const char* kTriggers = "h2.triggers";  // registry: the table the Lua part returns
constexpr size_t kMaxKeys = 64;                   // keys of one frame

double g_gameTime;  // seconds the world ran on this map
double g_last;      // NowMs of the last update
std::vector<int> g_keys;

// The Lua part: h2.time, h2.after / every, h2.zone; returns {update, clear,
// setowner}. Runs in the sandbox like any script (its own functions only).
const char* const kLua = R"lua(
local report = ...
local T = {}
local now, owner, lastCheck = 0, "", -1
local timers, zones = {}, {}

local function call(what, fn, ...)
  if fn then
    local ok, err = pcall(fn, ...)
    if not ok then report(what .. ": " .. tostring(err)) end
  end
end

local function number(v, what)
  if type(v) ~= "number" or v ~= v or v == math.huge or v == -math.huge or math.abs(v) > 1e7 then
    error(what .. ": a number expected", 3)
  end
  return v
end

function h2.time() return now end

local Timer = {}
Timer.__index = Timer
function Timer:cancel() self._dead = true end
function Timer:active() return not self._dead end

local function newTimer(seconds, fn, every, count)
  if type(fn) ~= "function" then error("a function expected", 3) end
  local t = setmetatable({_at = now + seconds, _fn = fn, _every = every, _left = count, _owner = owner}, Timer)
  timers[#timers + 1] = t
  return t
end

function h2.after(seconds, fn)
  seconds = number(seconds, "h2.after")
  return newTimer(math.max(seconds, 0), fn)
end

function h2.every(seconds, fn, count)
  seconds = number(seconds, "h2.every")
  if seconds < 0.01 then error("h2.every: at least 0.01 seconds", 2) end
  if count ~= nil then
    count = number(count, "h2.every")
    if count < 1 then error("h2.every: count at least 1", 2) end
  end
  return newTimer(seconds, fn, seconds, count)
end

local Zone = {}
Zone.__index = Zone

function Zone:remove() self._dead = true end

-- the centre of a sphere zone now (an object's zone moves with it)
local function centre(z)
  if z._object then
    local ok, x, y, zz = pcall(z._object.pos, z._object)
    if not ok then return nil end
    return x, y, zz
  end
  return z._x, z._y, z._z
end

function Zone:contains(x, y, zz)
  x, y, zz = number(x, "contains"), number(y, "contains"), number(zz, "contains")
  if self._min then
    return x >= self._min[1] and x <= self._max[1] and y >= self._min[2] and y <= self._max[2]
       and zz >= self._min[3] and zz <= self._max[3]
  end
  local cx, cy, cz = centre(self)
  if not cx then return false end
  local dx, dy, dz = x - cx, y - cy, zz - cz
  return dx * dx + dy * dy + dz * dz <= self._r * self._r
end

function Zone:inside(obj)
  local ok, x, y, zz = pcall(obj.pos, obj)
  return ok and self:contains(x, y, zz) or false
end

function h2.zone(spec)
  if type(spec) ~= "table" then error("h2.zone{...}: a table expected", 2) end
  local z = setmetatable({_owner = owner, _inside = {}, _who = spec.who or "hitman",
                          _enter = spec.on_enter, _leave = spec.on_leave}, Zone)
  for _, f in ipairs({"on_enter", "on_leave"}) do
    if spec[f] ~= nil and type(spec[f]) ~= "function" then error("h2.zone: " .. f .. " is not a function", 2) end
  end
  if spec.min or spec.max then
    if type(spec.min) ~= "table" or type(spec.max) ~= "table" then error("h2.zone: min and max: {x, y, z}", 2) end
    z._min, z._max = {}, {}
    for i = 1, 3 do
      local a, b = number(spec.min[i], "h2.zone min"), number(spec.max[i], "h2.zone max")
      z._min[i], z._max[i] = math.min(a, b), math.max(a, b)
    end
  else
    z._r = number(spec.radius, "h2.zone radius")
    if z._r <= 0 then error("h2.zone: radius above 0", 2) end
    if spec.object ~= nil then
      if type(spec.object) ~= "table" or not spec.object.pos then error("h2.zone: object is not an object", 2) end
      z._object = spec.object
    else
      z._x, z._y, z._z = number(spec.x, "h2.zone x"), number(spec.y, "h2.zone y"), number(spec.z, "h2.zone z")
    end
  end
  local w = z._who
  if w ~= "hitman" and w ~= "characters" and type(w) ~= "table" then
    error('h2.zone: who = "hitman", "characters", an object or a list of objects', 2)
  end
  zones[#zones + 1] = z
  return z
end

-- the objects a zone watches now
local function candidates(z)
  local w, list = z._who, {}
  if w == "hitman" or w == "characters" then
    if w == "characters" then
      local ok, all = pcall(h2.objects, "ZActor")
      if ok and type(all) == "table" then list = all end
    end
    local ok, p = pcall(h2.player)
    if ok and p then list[#list + 1] = p end
  elseif w.pos then
    list[1] = w
  else
    for _, o in ipairs(w) do list[#list + 1] = o end
  end
  return list
end

local function check(z)
  if not z._min and not centre(z) then return end  -- its object is gone
  local seen = {}
  for _, o in ipairs(candidates(z)) do
    local ok, path = pcall(o.path, o)
    if ok and path and not seen[path] then
      seen[path] = true
      local inside = z:inside(o)
      if inside and not z._inside[path] then
        z._inside[path] = o
        call("zone on_enter", z._enter, z, o)
      elseif not inside and z._inside[path] then
        z._inside[path] = nil
        call("zone on_leave", z._leave, z, o)
      end
    end
    if z._dead then return end
  end
  for path, o in pairs(z._inside) do
    if not seen[path] then  -- gone from the scene or from the list
      z._inside[path] = nil
      call("zone on_leave", z._leave, z, o)
    end
  end
end

function T.update(time)
  now = time
  local due = timers
  timers = {}
  local keep = {}
  for _, t in ipairs(due) do
    if not t._dead and now >= t._at then
      call("timer", t._fn, t)
      if t._every and not t._dead then
        if t._left then
          t._left = t._left - 1
          if t._left <= 0 then t._dead = true end
        end
        t._at = t._at + t._every
        if t._at <= now then t._at = now + t._every end
      else
        t._dead = true
      end
    end
    if not t._dead then keep[#keep + 1] = t end
  end
  for _, t in ipairs(timers) do keep[#keep + 1] = t end  -- made by the callbacks
  timers = keep
  if now - lastCheck >= 0.1 or lastCheck < 0 or now < lastCheck then
    lastCheck = now
    local list = {}
    for _, z in ipairs(zones) do
      if not z._dead then list[#list + 1] = z end
    end
    zones = list
    for i = 1, #list do
      if not list[i]._dead then check(list[i]) end
    end
  end
end

function T.clear(who)
  for _, t in ipairs(timers) do
    if who == nil or t._owner == who then t._dead = true end
  end
  for _, z in ipairs(zones) do
    if who == nil or z._owner == who then z._dead = true end
  end
  if who == nil then now, lastCheck = 0, -1 end
end

-- a subtitle line, away after `seconds` (default 5) unless another came
local subtitleTimer
function h2.subtitle(text, seconds)
  if text ~= nil and type(text) ~= "string" then error("h2.subtitle: a text expected", 2) end
  if subtitleTimer then subtitleTimer:cancel() subtitleTimer = nil end
  local ok = h2._subtitle(text or "")
  if text and text ~= "" then
    seconds = seconds and number(seconds, "h2.subtitle") or 5
    if seconds > 0 then
      subtitleTimer = newTimer(seconds, function() subtitleTimer = nil h2._subtitle("") end)
    end
  end
  return ok
end

function T.setowner(o) owner = o end

return T
)lua";

int Report(lua_State* L)
{
    ConsolePrint(LineKind::Error, luaL_tolstring(L, 1, nullptr));
    return 0;
}

bool OnMap()
{
    const std::string& s = CurrentScene();
    return s.size() > 6 && _stricmp(s.c_str() + s.size() - 6, "__MAIN") == 0 && engine::SceneDataValid();
}

// T[fn](args): `push` pushes the arguments, returns their count
template <typename Push>
void CallLua(const char* fn, Push push)
{
    lua_State* L = State();
    if (!L)
        return;
    int top = lua_gettop(L);
    if (lua_getfield(L, LUA_REGISTRYINDEX, kTriggers) == LUA_TTABLE && lua_getfield(L, -1, fn) == LUA_TFUNCTION) {
        int n = push(L);
        if (PCall(L, n, 0, 0) != LUA_OK) {
            const char* err = lua_tostring(L, -1);
            ConsolePrint(LineKind::Error, std::string("triggers: ") + (err ? err : "error"));
        }
    }
    lua_settop(L, top);
}

std::string KeyName(int vk)
{
    char buf[16];
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return std::string(1, char(vk));
    if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(buf, sizeof(buf), "F%d", vk - VK_F1 + 1);
        return buf;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        snprintf(buf, sizeof(buf), "Numpad%d", vk - VK_NUMPAD0);
        return buf;
    }
    switch (vk) {
    case VK_SPACE: return "Space";
    case VK_RETURN: return "Enter";
    case VK_ESCAPE: return "Escape";
    case VK_TAB: return "Tab";
    case VK_BACK: return "Backspace";
    case VK_SHIFT: return "Shift";
    case VK_CONTROL: return "Ctrl";
    case VK_MENU: return "Alt";
    case VK_CAPITAL: return "CapsLock";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    case VK_PAUSE: return "Pause";
    case VK_MULTIPLY: return "Multiply";
    case VK_ADD: return "Add";
    case VK_SUBTRACT: return "Subtract";
    case VK_DIVIDE: return "Divide";
    case VK_DECIMAL: return "Decimal";
    }
    snprintf(buf, sizeof(buf), "VK_%02X", vk & 0xFF);
    return buf;
}

void ClearAll()
{
    CallLua("clear", [](lua_State* L) {
        lua_pushnil(L);
        return 1;
    });
}

bool OnKey(int vk)
{
    if (OnMap() && g_keys.size() < kMaxKeys)
        g_keys.push_back(vk);
    return false;  // the game gets it too
}

} // namespace

void InstallTriggers()
{
    lua_State* L = State();
    if (luaL_loadbuffer(L, kLua, strlen(kLua), "=triggers") != LUA_OK) {
        Log("triggers: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    lua_pushcfunction(L, Report);
    if (PCall(L, 1, 1, 0) != LUA_OK || !lua_istable(L, -1)) {
        Log("triggers: %s", lua_tostring(L, -1) ? lua_tostring(L, -1) : "no table");
        lua_pop(L, 1);
        return;
    }
    lua_setfield(L, LUA_REGISTRYINDEX, kTriggers);
    overlay::AddKeyHandler(OnKey);
    OnSceneLeaving([](const std::string&) {
        ClearAll();  // they belong to the map being left
        g_keys.clear();
    });
    OnScenePushed([](const std::string&) { g_gameTime = 0.0; });
    g_last = NowMs();
}

void UpdateTriggers()
{
    double now = NowMs();
    double dt = std::clamp((now - g_last) / 1000.0, 0.0, 0.25);  // no jump after a load
    g_last = now;
    if (!OnMap()) {
        g_keys.clear();
        return;
    }
    if (!editor::ScenePaused())
        g_gameTime += dt * editor::GameSpeed();
    std::vector<int> keys;
    keys.swap(g_keys);
    for (int vk : keys)
        FireKey(vk, KeyName(vk));
    CallLua("update", [](lua_State* L) {
        lua_pushnumber(L, g_gameTime);
        return 1;
    });
}

void SetTriggerOwner(const std::string& owner)
{
    CallLua("setowner", [&](lua_State* L) {
        lua_pushstring(L, owner.c_str());
        return 1;
    });
}

void ClearTriggers(const std::string& owner)
{
    CallLua("clear", [&](lua_State* L) {
        lua_pushstring(L, owner.c_str());
        return 1;
    });
}

} // namespace h2::script
