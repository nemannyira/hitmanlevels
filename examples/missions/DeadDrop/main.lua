-- Dead Drop (C1-1, emptied map): a package lies in the garage yard, two
-- guards watch it. Take it and leave it at the postman's corner by the road
-- south in four minutes. Seen twice, the drop is off.
--
-- Made in the editor: Empty map (Level tab), the two guards (NPCs tab:
-- BodyguardMafia_01 with BaseGuard), the package (Models tab > items >
-- KeyRoom). This script tells the story and keeps the score.

local DROP = { 3547, 5, -10797 }    -- the postman's corner
local TIME_LIMIT = 240

h2.state.stage = h2.state.stage or 1  -- 1 take it, 2 bring it, 3 over
h2.state.seen = h2.state.seen or 0

-- the package: the level's KeyRoom nearest the yard
local package, best = nil, math.huge
for _, o in ipairs(h2.findall("KeyRoom")) do
  local x, _, z = o:pos()
  local d = (x - 2700) ^ 2 + (z + 5500) ^ 2
  if d < best then package, best = o, d end
end
if not package then error("Dead Drop: the package is not on the map") end

h2.briefing("Our man in the Don's house left a package in the garage yard. Two of the Don's men " ..
            "guard it. Take it and leave it at the postman's corner by the road south before the " ..
            "postman comes: four minutes. If they see you twice, the drop is off.")

local function lines()
  local s = h2.state.stage
  h2.objective(1, "Take the package from the garage yard", s >= 2)
  h2.objective(2, "Leave it at the postman's corner", s >= 3 and not h2.state.failed)
  h2.objective(3, "Do not be seen twice", false)
end
lines()

local function fail(why)
  if h2.state.stage == 3 then return end
  h2.state.stage, h2.state.failed = 3, true
  lines()
  h2.subtitle(why, 6)
  h2.after(3, function() h2.mission("fail") end)
end

-- the time limit, kept with saved games
local deadline = h2.time() + (h2.state.left or TIME_LIMIT)
h2.on("save", function() h2.state.left = math.max(0, deadline - h2.time()) end)
local warned = false
h2.every(1, function(t)
  if h2.state.stage == 3 then t:cancel() return end
  local left = deadline - h2.time()
  if not warned and left < 60 then
    warned = true
    h2.info("One minute until the postman comes.", "warning")
  end
  if left <= 0 then
    t:cancel()
    fail("The postman is here. The drop is off.")
  end
end)

if h2.state.stage == 1 then
  h2.subtitle("The package lies in the garage yard. Four minutes.", 6)
end

h2.on("pickup", function(e)
  if h2.state.stage == 1 and e.object and e.object:path() == package:path() then
    h2.state.stage = 2
    lines()
    h2.subtitle("Got it. Now the postman's corner, by the road south.", 6)
  end
end)

-- seen: a guard reacts to Hitman (the "spotted" / "hostile" events, once a second each)
local lastSeen = -100
local function seen()
  if h2.state.stage == 3 or h2.time() - lastSeen < 10 then return end
  lastSeen = h2.time()
  h2.state.seen = h2.state.seen + 1
  if h2.state.seen >= 2 then
    fail("They saw you again. The drop is off.")
  else
    h2.info("They saw you. Once more and the drop is off.", "warning")
  end
end
h2.on("spotted", seen)
h2.on("hostile", seen)

h2.zone{ x = DROP[1], y = DROP[2], z = DROP[3], radius = 250,
  on_enter = function()
    if h2.state.stage == 2 then
      h2.state.stage = 3
      lines()
      h2.objective(3, "Do not be seen twice", true)
      h2.subtitle("The package is in the box. Job done.", 5)
      h2.after(3, function() h2.mission("complete") end)
    elseif h2.state.stage == 1 then
      h2.info("The package is still in the garage yard.", "hint")
    end
  end }
