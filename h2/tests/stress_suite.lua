-- Filling the map: characters, items and copies until the editor refuses
-- (the engine's buffers are sized at loading; full, the game would go down).
-- Run with run_suite.py on a freshly loaded map, the editor closed.
local R = {pass = 0, fail = 0, log = {}, done = false, pending = 0}
SUITE = R
local function report(okay, name, info)
  if okay then R.pass = R.pass + 1
  else R.fail = R.fail + 1 R.log[#R.log + 1] = "FAIL " .. name .. (info and (": " .. tostring(info)) or "") end
end

local hx, hy, hz = h2.player():pos()
local function fill(name, max, make)
  local n, err = 0, nil
  for i = 1, max do
    local ok, e = pcall(make, i)
    if not ok then err = e break end
    n = n + 1
  end
  report(err and tostring(err):find("no room") ~= nil, name .. " refused when full", tostring(n) .. " " .. tostring(err))
  return n
end

local guards = {}
fill("armed characters", 200, function(i)
  guards[#guards + 1] = h2.npc("BodyguardMafia_01", -4176 + (i % 10) * 60, -20, -6682 + (i // 10) * 60, 0)
end)
fill("items", 5000, function() h2.item("KeyRoom", hx, hy + 5, hz) end)
local crate
for _, o in ipairs(h2.objects("ZSTDOBJ")) do if o:mesh() and o:mesh() ~= 0 then crate = o break end end
fill("copies", 20000, function() crate:spawn(hx, hy + 300, hz) end)
fill("points", 20000, function() h2.point(hx, hy, hz) end)
R.pending = R.pending + 1
h2.after(1, function() for _, g in ipairs(guards) do g:kill() end end)
h2.after(8, function()
  local dead = 0
  for _, g in ipairs(guards) do if not g:alive() then dead = dead + 1 end end
  report(dead == #guards, "all killed (weapons dropped)", dead .. "/" .. #guards)
  local nodes, bytes = h2.capacity()
  report(nodes > 0 and bytes > 0, "room left for the game itself", nodes .. " " .. bytes)
  R.pending = R.pending - 1
  R.done = true
end)
return "stress suite started"
