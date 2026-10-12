-- The Informant (C1-1): a delivery boy who saw too much waits on the east
-- road. Find him and take him to the postman's corner, where a car picks him
-- up. He follows you; keep close and keep him alive.
--
-- Made in the editor: the informant (NPCs tab: DeliveryBoy, no behaviour),
-- a point at the meeting place. This script makes him follow Hitman.

local MEET = "Point+1"             -- where the car waits
local LOST_DISTANCE = 4000         -- cm: farther than this he is on his own ...
local LOST_TIME = 20               -- ... and after this long he runs off

h2.basemission(false)              -- C1-1's own targets do not end this mission
for slot = 1, 4 do h2.objective(slot, false) end

h2.state.stage = h2.state.stage or 1   -- 1 find him, 2 bring him, 3 over

local boy = h2.find("NPC_DeliveryBoy_2")
local meet = h2.find(MEET)
if not boy or not meet then error("The Informant: the informant or the meeting point is missing") end
local mx, my, mz = meet:pos()

h2.briefing("A delivery boy saw the Don's men bury a body. He waits for you on the east road, " ..
            "scared to death. Take him to the postman's corner by the south road: a car waits " ..
            "there. He will follow you. Keep him close and keep him alive.")

local function lines()
  local s = h2.state.stage
  h2.objective(5, "Find the informant on the east road", s >= 2)
  h2.objective(6, "Bring him to the postman's corner", s >= 3 and not h2.state.failed)
end
lines()

local over = false
local function finish(ok, text)
  if over then return end
  over = true
  h2.state.stage = 3
  h2.state.failed = not ok
  lines()
  h2.subtitle(text, 6)
  h2.after(3, function() h2.mission(ok and "complete" or "fail") end)
end

local function dist2(ax, az, bx, bz) return (ax - bx) ^ 2 + (az - bz) ^ 2 end

-- stage 1: he waits; Hitman comes near
h2.zone{ object = boy, radius = 400,
  on_enter = function()
    if h2.state.stage ~= 1 then return end
    h2.state.stage = 2
    lines()
    h2.subtitle("\"You're the one? Get me out of here!\" He follows you.", 5)
    boy:lookat(h2.player())
  end }

-- stage 2: he follows; too far for too long and he runs off
local farSince, warnedAt
h2.every(1, function(t)
  if over then t:cancel() return end
  if not boy:alive() then return end
  if h2.state.stage ~= 2 then return end
  local hx, hy, hz = h2.player():pos()
  local bx, _, bz = boy:pos()
  local d2 = dist2(hx, hz, bx, bz)
  if d2 > LOST_DISTANCE ^ 2 then
    farSince = farSince or h2.time()
    local left = LOST_TIME - (h2.time() - farSince)
    if left <= 0 then
      boy:moveset("Run")
      boy:walkto(h2.walkable(5460, 3, -2224, 400))
      finish(false, "The informant lost you and ran off.")
      return
    elseif math.ceil(left / 5) < (warnedAt or math.huge) then  -- once every 5 seconds
      warnedAt = math.ceil(left / 5)
      h2.info(string.format("The informant is lost: go back to him (%d s).", math.ceil(left)), "warning")
    end
    return
  end
  farSince, warnedAt = nil, nil
  if d2 > 300 ^ 2 then                     -- keeps a few steps behind
    local x, y, z = h2.walkable(hx, hy, hz, 400)
    if x then
      boy:moveset(d2 > 1000 ^ 2 and "Run" or "Walk")
      boy:walkto(x, y, z)
    end
  end
  if dist2(bx, bz, mx, mz) < 350 ^ 2 then
    finish(true, "\"Thank you, mister.\" He gets into the car.")
  end
end)

h2.on("death", function(who)
  if who:path() == boy:path() and not over then
    finish(false, "The informant is dead.")
  end
end)

if h2.state.stage == 1 then
  h2.subtitle("The informant waits on the east road.", 6)
  h2.after(8, function()
    if h2.state.stage == 1 then h2.info("The east road: past the delivery van, by the outer wall.", "hint") end
  end)
end
