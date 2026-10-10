-- In-game test suite of the Lua API (C1-1, the editor closed, Hitman at the
-- level's start). Run with tools/tests/run_suite.py; results in SUITE.
-- Every check is protected: a failing call is reported, the suite goes on.
local R = {pass = 0, fail = 0, log = {}, done = false, pending = 0}
SUITE = R

local function report(okay, name, info)
  if okay then
    R.pass = R.pass + 1
  else
    R.fail = R.fail + 1
    R.log[#R.log + 1] = "FAIL " .. name .. (info and (": " .. tostring(info)) or "")
  end
end

local function check(name, fn)
  local okay, res, info = pcall(fn)
  if not okay then report(false, name, res) else report(res ~= false, name, info) end
end

local function rejects(name, fn)  -- bad input: an error, not a crash or silence
  local okay = pcall(fn)
  report(not okay, name, okay and "accepted" or nil)
end

local function near(a, b, tol) return math.abs(a - b) <= (tol or 1) end

local function later(seconds, name, fn)
  R.pending = R.pending + 1
  h2.after(seconds, function()
    check(name, fn)
    R.pending = R.pending - 1
  end)
end

-- ---------------------------------------------------------------- basics
check("scene", function() return h2.scene():find("C1%-1__MAIN") ~= nil end)
check("fps", function() return h2.fps() > 0 end)
check("resolution", function() local w, h = h2.resolution() return w > 0 and h > 0 end)
check("log", function() h2.log("suite: log line") return true end)
check("time runs", function() return h2.time() >= 0 end)
check("speed", function()
  local was = h2.speed()
  h2.speed(2) local two = h2.speed()
  h2.speed(was)
  return near(two, 2, 0.01)
end)
check("speed nan is ignored", function() h2.speed(0/0) local s = h2.speed() h2.speed(1) return s == s and s > 0 end)
check("god toggle", function() h2.god(true) local a = h2.god() h2.god(false) return a and not h2.god() end)
check("invisible toggle", function() h2.invisible(true) local a = h2.invisible() h2.invisible(false) return a and not h2.invisible() end)
check("freeze toggle", function() h2.freeze(true) local a = h2.freeze() h2.freeze(false) return a and not h2.freeze() end)
check("camera", function()
  h2.freecam(true)
  h2.setcampos(100, 200, 300)
  local x, y, z = h2.campos()
  h2.setcamrot(45, -10)
  local yw, p = h2.camrot()
  h2.camlookat(0, 0, 0)
  h2.freecam(false)
  return near(x, 100) and near(y, 200) and near(z, 300) and near(yw, 45, 0.5) and near(p, -10, 0.5)
end)
rejects("setcampos nan", function() h2.setcampos(0/0, 0, 0) end)

-- ---------------------------------------------------------------- objects
local hitman = h2.player()
check("player", function() return hitman and hitman:class() == "ZHITMAN2" and hitman:alive() end)
check("find by name", function() return h2.find("BodyguardMafia_FrontGate_00") ~= nil end)
check("find missing", function() return h2.find("No_Such_Object_Name") == nil end)
check("findall", function() return #h2.findall("Ground") > 5 end)
check("objects ZActor", function() return #h2.objects("ZActor") > 10 end)
check("objects ZItemTemplate", function() return #h2.objects("ZItemTemplate") > 3 end)
check("object basics", function()
  local o = h2.find("BodyguardMafia_FrontGate_00")
  local x, y, z = o:pos()
  local yaw = o:rot()
  return o:name() == "BodyguardMafia_FrontGate_00" and o:path():find("FrontGate") and o:isa("ZGROUP")
     and o:valid() and type(x) == "number" and type(yaw) == "number" and #o:children() >= 1
     and o:children()[1]:parent() == o and type(o:flags()) == "number" and o:addr() ~= nil
end)
check("tostring / eq", function()
  local a, b = h2.find("BodyguardMafia_FrontGate_00"), h2.find("BodyguardMafia_FrontGate_00")
  return a == b and tostring(a):find("object%(") ~= nil
end)
rejects("method on a non-object", function() local f = getmetatable(hitman).__index.pos f({}) end)
rejects("method on a fake path", function()
  local f = getmetatable(hitman).__index.pos f({_path = "ROOT/no/such"}) end)

-- a plain object: move, rotate, hide, spawn a copy, destroy it
local crate
for _, o in ipairs(h2.objects("ZSTDOBJ")) do
  if o:mesh() and o:mesh() ~= 0 and not o:hidden() then crate = o break end
end
check("plain object found", function() return crate ~= nil end)
if crate then
  check("setpos / setrot / hide / show", function()
    local x, y, z = crate:pos()
    local yw = crate:rot()
    crate:setpos(x + 10, y, z) local x2 = crate:pos()
    crate:setpos(x, y, z)
    crate:setrot(yw + 30) local y2 = crate:rot()
    crate:setrot(yw)
    crate:hide() local h = crate:hidden()
    crate:show()
    return near(x2, x + 10) and near((y2 - yw + 540) % 360 - 180, 30, 0.5) and h and not crate:hidden()
  end)
  rejects("setpos nan", function() crate:setpos(0/0, 0, 0) end)
  rejects("setpos huge", function() crate:setpos(1e30, 0, 0) end)
  check("spawn / destroy", function()
    local x, y, z = hitman:pos()
    local c = crate:spawn(x + 300, y, z, 0)
    local s = c:spawned()
    c:destroy()
    return s and not c:valid()
  end)
  check("h2.spawn", function()
    local x, y, z = hitman:pos()
    local c = h2.spawn(crate, x + 300, y, z)
    local ok = c:valid()
    c:destroy()
    return ok
  end)
  check("mesh_object", function()
    local x, y, z = hitman:pos()
    local m = h2.mesh_object(crate:mesh(), x + 400, y, z)
    local ok = m and m:valid()
    if m then m:destroy() end
    return ok
  end)
end
rejects("destroy a map object", function() h2.find("BodyguardMafia_FrontGate_00"):destroy() end)

check("point", function()
  local p = h2.point(0, 100, 0)
  local x, y, z = p:pos()
  return near(y, 100)
end)

-- items
check("item", function()
  local x, y, z = hitman:pos()
  local it = h2.item("KeyRoom", x + 50, y + 5, z)
  return it and it:valid() and it:spawned()
end)
rejects("item bad template", function() h2.item("NoSuchTemplate", 0, 0, 0) end)

-- lists (routes)
local list
for _, cls in ipairs({"ZLIST", "ZList", "ZGEOMLIST"}) do
  local ok, all = pcall(h2.objects, cls)
  if ok and all and #all > 0 then
    for _, l in ipairs(all) do
      local ok2, pts = pcall(l.points, l)
      if ok2 and pts and #pts > 0 then list = l break end
    end
  end
  if list then break end
end
check("route list found", function() return list ~= nil end)
if list then
  check("points / setpoints", function()
    local pts = list:points()
    list:setpoints(pts)
    return #list:points() == #pts
  end)
end

-- ---------------------------------------------------------------- characters
check("npcs", function() local n = h2.npcs() return #n > 5 and n[1].record and n[1].object end)
check("behaviours", function() return #h2.behaviours() >= 1 and #h2.behaviours(true) > #h2.behaviours() end)
local gx, gy, gz = -4176, -20, -6682
local npc
check("npc without behaviour", function()
  npc = h2.npc("BodyguardMafia_01", gx, gy, gz, 0, false)
  return npc and npc:alive() and npc:behaviour() == nil
end)
local guard
check("npc with behaviour", function()
  guard = h2.npc("BodyguardMafia_01", gx + 150, gy, gz, 0)
  return guard and guard:behaviour() ~= nil and type(guard:state()) == "string"
end)
rejects("npc bad model", function() h2.npc("NoSuchModel_99", 0, 0, 0) end)
rejects("npc nan", function() h2.npc("BodyguardMafia_01", 0/0, 0, 0) end)
if npc then
  check("moveset / lookat / faceto / animation", function()
    npc:moveset("Walk")
    npc:lookat(hitman) npc:lookat(nil)
    npc:faceto(gx + 100, gy, gz)
    return npc:animation("NoSuchAnimation") == false
  end)
  check("walkto on the network", function() return npc:walkto(-4637, -20, -6353) == true end)
  later(8, "walked", function()
    local x, _, z = npc:pos()
    return (x + 4637) ^ 2 + (z + 6353) ^ 2 < 250 ^ 2, string.format("%.0f %.0f", x, z)
  end)
  rejects("walkto a non-character", function() npc.walkto(crate or hitman, 0, 0, 0) end)
end
if guard then
  check("carry", function()
    return guard:carry("KeyDoor") and #guard:carrying() == 1
  end)
  later(1, "kill (with a behaviour)", function() guard:kill() return true end)
  later(4, "dead and dropped", function()
    local dropped = false
    for _, o in ipairs(h2.objects("ZItem")) do
      if o:spawned() and o:path():find("KeyDoor") then dropped = true end
    end
    return not guard:alive() and dropped and #guard:carrying() == 0
  end)
end

-- ---------------------------------------------------------------- mission, text
check("objective", function()
  h2.objective(1, "Suite line one")
  h2.objective(1, nil, true)
  h2.objective(2, false)
  return type(h2.objective(1)) == "string"
end)
rejects("objective slot 99", function() h2.objective(99, "x") end)
check("briefing", function() h2.briefing("Suite briefing") h2.briefing("") return true end)
check("basemission", function()
  local was = h2.basemission()
  h2.basemission(false) local off = h2.basemission()
  h2.basemission(was)
  return off == false
end)
check("info / subtitle", function() return h2.info("suite info") and h2.subtitle("suite subtitle", 1) end)
rejects("info bad kind", function() h2.info("x", "shout") end)
check("music", function() local zones, tracks = h2.music() return type(zones) == "table" and type(tracks) == "table" end)
check("sky", function() return h2.sky() ~= nil or true end)
check("onpath", function()
  return h2.onpath(-4176, -20, -6682) == true and h2.onpath(7285, 974, -7584) == false
end)
check("messages", function() return #h2.messages() > 100 end)

-- ---------------------------------------------------------------- Hitman
check("health", function()
  local was = h2.health()
  h2.health(was - 1) local a = h2.health()
  h2.health(was)
  return near(a, was - 1, 0.01)
end)
rejects("health 0", function() h2.health(0) end)
rejects("health huge", function() h2.health(1e9) end)
check("give", function()
  local g = h2.give("GunHardballer")
  return g and g:valid()
end)
rejects("give bad template", function() h2.give("NoSuchTemplate") end)

-- ---------------------------------------------------------------- events, timers, zones, state
local seen = {}
h2.on("frame", function() seen.frame = true end)
h2.on("suite_never", function() end)
h2.off("suite_never")
local every = 0
h2.every(0.2, function() every = every + 1 end, 3)
local afterFired = false
local t = h2.after(0.3, function() afterFired = true end)
local cancelled = h2.after(0.3, function() seen.cancelledRan = true end)
cancelled:cancel()
local hx, hy, hz = hitman:pos()
local entered = false
local zone = h2.zone{x = hx, y = hy, z = hz, radius = 300, on_enter = function() entered = true end}
local boxIn = false
h2.zone{min = {hx - 100, hy - 200, hz - 100}, max = {hx + 100, hy + 300, hz + 100}, on_enter = function() boxIn = true end}
rejects("zone without radius", function() h2.zone{x = 0, y = 0, z = 0} end)
rejects("every too fast", function() h2.every(0.001, function() end) end)
later(2, "frame event", function() return seen.frame == true end)
later(2, "after / cancel", function() return afterFired and not seen.cancelledRan and not t:active() end)
later(2, "every n times", function() return every == 3, every end)
later(2, "zone sphere / box enter", function() return entered and boxIn and zone:inside(hitman) end)
check("state", function()
  h2.state.suite = {n = 1, list = {1, 2, 3}, s = "x"}
  return h2.state.suite.list[3] == 3
end)

-- the end
later(10, "suite end", function() return true end)
h2.after(10.5, function() R.done = true end)
return "suite started"
