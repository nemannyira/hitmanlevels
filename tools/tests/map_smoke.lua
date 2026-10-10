-- Map-independent checks for any loaded map (tools/tests/run_maps.py runs it
-- on every map). Results in SUITE (as api_suite.lua).
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
local function later(seconds, name, fn)
  R.pending = R.pending + 1
  h2.after(seconds, function() check(name, fn) R.pending = R.pending - 1 end)
end

local hitman = h2.player()
check("player", function() return hitman ~= nil and hitman:alive() end)
local hx, hy, hz = hitman:pos()
check("objects", function() return #h2.objects("ZGEOM") > 50 and #h2.objects("ZROOM") > 1 end)
check("characters listed", function() return type(h2.npcs()) == "table" end)
check("messages", function() return #h2.messages() > 50 end)
check("music", function() local z, t = h2.music() return type(z) == "table" and type(t) == "table" end)
check("subtitle / info", function() h2.subtitle("map smoke", 1) h2.info("map smoke") return true end)
check("health", function() local h = h2.health() h2.health(h) return h > 0 end)

-- a walkable spot near Hitman: the path network looked at around him
local spot
check("path network", function()
  local on = h2.onpath(hx, hy, hz)
  if on == nil then return true, "no network" end
  for r = 0, 2000, 100 do
    for a = 0, 330, 30 do
      local x, z = hx + r * math.cos(math.rad(a)), hz + r * math.sin(math.rad(a))
      for dy = 200, -400, -50 do
        if h2.onpath(x, hy + dy, z) then spot = {x, hy + dy, z} break end
      end
      if spot then break end
    end
    if spot then break end
  end
  return true
end)

-- a character of the map's own kind, made there, walked, killed
local model
for _, n in ipairs(h2.npcs()) do
  if n.model and n.model ~= "" then model = n.model break end
end
local npc, guard
if model and spot then
  check("npc " .. model, function()
    npc = h2.npc(model, spot[1], spot[2], spot[3], 0, false)
    return npc and npc:alive()
  end)
  check("npc with its behaviour", function()
    guard = h2.npc(model, spot[1] + 60, spot[2], spot[3])
    return guard ~= nil
  end)
  if npc then
    check("walkto", function() return npc:walkto(spot[1], spot[2], spot[3]) ~= nil end)
    check("setpos / settle", function() npc:setpos(spot[1] + 30, spot[2], spot[3]) return true end)
  end
  if guard then later(1, "kill early (weapon drop)", function() return guard:kill() ~= nil end) end
  if npc then later(2, "kill", function() return npc:kill() ~= nil end) end
  later(5, "dead", function() return (not npc or not npc:alive()) end)
end

-- items, a copy of a model, a point
local tmpl = h2.objects("ZItemTemplate")[1]
if tmpl then
  check("item " .. tmpl:name(), function() return h2.item(tmpl, hx + 50, hy + 5, hz):valid() end)
  check("give " .. tmpl:name(), function() return h2.give(tmpl:name()) ~= nil end)
end
for _, o in ipairs(h2.objects("ZSTDOBJ")) do
  if o:mesh() and o:mesh() ~= 0 and not o:hidden() then
    check("spawn / destroy", function() local c = o:spawn(hx, hy + 300, hz) c:destroy() return true end)
    break
  end
end
check("point", function() return h2.point(hx, hy, hz):valid() end)

local ticks = 0
h2.every(0.25, function() ticks = ticks + 1 end, 4)
later(3, "timers", function() return ticks == 4 end)
h2.after(6, function() R.done = true end)
return "map smoke started"
