-- Sandbox and robustness checks (run with run_suite.py, the editor closed).
-- Each check tries something a hostile level script could do; it must fail
-- safely (an error, a limit) and the game must go on.
local R = {pass = 0, fail = 0, log = {}, done = false, pending = 0}
SUITE = R
local function report(okay, name, info)
  if okay then R.pass = R.pass + 1
  else R.fail = R.fail + 1 R.log[#R.log + 1] = "FAIL " .. name .. (info and (": " .. tostring(info)) or "") end
end
local function check(name, fn)
  local okay, res, info = pcall(fn)
  if not okay then report(false, name, res) else report(res ~= false, name, info) end
end
local function blocked(name, fn)  -- must raise an error
  local okay, err = pcall(fn)
  report(not okay, name, okay and "allowed" or nil)
end

-- no way to the system
check("no io", function() return io == nil end)
check("no package / require", function() return package == nil and require == nil end)
check("no debug", function() return debug == nil end)
check("no dofile / loadfile", function() return dofile == nil and loadfile == nil end)
check("os trimmed", function()
  return os.execute == nil and os.exit == nil and os.getenv == nil and os.remove == nil
     and os.rename == nil and os.tmpname == nil and os.setlocale == nil
end)
check("no string.dump", function() return string.dump == nil end)
blocked("load binary chunk", function()
  assert(load("\27LuaT\0\25\147\r\n\26\n\4\8\8xV\0\0\0\0\0\0\0\0\0\0\0(w@", "x", "b"))
end)
check("load text only", function() local f = load("return 1 + 1") return f and f() == 2 end)
blocked("load binary through env trick", function() assert(load(string.char(27) .. "Lua", "x", "bt")) end)
check("no h2.mem without LuaMemory", function() return h2.mem == nil end)
blocked("finalizer", function() setmetatable({}, {__gc = function() while true do end end}) end)
check("setmetatable still works", function() return getmetatable(setmetatable({}, {__index = {a = 1}})).__index.a == 1 end)

check("xpcall still works", function()
  local ok, v = xpcall(function() error("e") end, function(m) return "handled" end)
  local ok2, a, b = xpcall(function(x, y) return x + y, "two" end, print, 1, 2)
  return ok == false and v == "handled" and ok2 and a == 3 and b == "two"
end)

-- limits: each in a call of its own (a stopped call cannot go on: every
-- instruction fails until it returns), checked afterwards
local S = {}
local function stopped(name, delay, fn)
  R.pending = R.pending + 1
  h2.after(delay, function()
    S[name] = "started"
    fn()
    S[name] = "finished"
  end)
  h2.after(delay + 4, function()
    report(S[name] == "started", name, S[name])
    R.pending = R.pending - 1
  end)
end
stopped("endless loop", 0.1, function() while true do end end)
stopped("endless loop caught by pcall in a loop", 0.2, function()
  while true do pcall(function() while true do end end) end
end)
stopped("endless coroutines", 0.3, function()
  while true do coroutine.resume(coroutine.create(function() while true do end end)) end
end)
stopped("endless xpcall handler", 0.4, function()
  while true do xpcall(function() error("x") end, function() while true do end end) end
end)
-- a runaway repeating timer is stopped once and removed
local runs = 0
h2.every(0.5, function() runs = runs + 1 while true do end end)
-- a runaway event handler is removed, a well-behaved one of the same event still runs
local good = 0
h2.on("frame", function() while true do end end)
h2.on("frame", function() good = good + 1 end)
R.pending = R.pending + 1
h2.after(7, function()
  report(runs == 1, "runaway timer removed after one stop", runs)
  report(good > 5, "other handlers of the event keep running", good)
  R.pending = R.pending - 1
end)
blocked("memory", function() local t = {} for i = 1, 1e9 do t[i] = string.rep("x", 1024) .. i end end)
check("memory back", function() collectgarbage() local t = {} for i = 1, 1000 do t[i] = i end return #t == 1000 end)
blocked("huge string", function() return string.rep("x", 2 ^ 31) end)
blocked("deep recursion", function() local function f(n) return f(n + 1) + 1 end return f(1) end)
check("after the limits the state works", function() return h2.time() >= 0 and #h2.objects("ZActor") > 0 end)
later_ok = true

-- bad values everywhere
local hitman = h2.player()
blocked("setpos inf", function() hitman:setpos(math.huge, 0, 0) end)
blocked("npc huge", function() h2.npc("BodyguardMafia_01", 1e8, 0, 0) end)
blocked("objective bad slot", function() h2.objective(0, "x") end)
blocked("objective table text", function() h2.objective(1, {}) end)
check("objective long text", function() h2.objective(3, string.rep("long objective ", 2000)) h2.objective(3, false) return true end)
check("briefing long text", function() h2.briefing(string.rep("briefing ", 20000)) h2.briefing("") return true end)
check("subtitle long text", function() return h2.subtitle(string.rep("s", 100000), 1) end)
check("info long text", function() return h2.info(string.rep("i", 100000)) end)
check("strange characters", function()
  h2.subtitle("\0\1\2\255 %s %n %d \\ \"", 1) h2.info("%s%s%s%n%n") h2.log("%s%n") print("%n%s") return true
end)
blocked("find with a fake object", function() local m = getmetatable(hitman).__index m.pos({_path = 1}) end)
blocked("event name huge", function() hitman:event(string.rep("e", 1e6)) end)
check("state cycles", function()
  local a = {} a.self = a
  h2.state.cycle = a
  return true
end)
check("many handlers", function() for i = 1, 10000 do h2.on("suite_many", function() end) end h2.off("suite_many") return true end)
check("many timers", function()
  local n = 0
  for i = 1, 5000 do h2.after(0.1, function() n = n + 1 end) end
  return true
end)
check("many zones", function()
  local x, y, z = hitman:pos()
  for i = 1, 500 do h2.zone{x = x, y = y, z = z, radius = 10, on_enter = function() end}:remove() end
  return true
end)

-- the metatables of strings and objects
check("string metatable change stays in the script", function()
  local mt = getmetatable("")
  local old = mt.__index
  mt.__index = function() error("boom") end
  local okay = pcall(function() return ("x"):upper() end)
  mt.__index = old
  return not okay and ("x"):upper() == "X"
end)

h2.state.cycle = nil
h2.after(9, function()
  check("timers still run after the stopped calls", function() return h2.time() > 0 end)
  R.done = true
end)
return "security suite started"
