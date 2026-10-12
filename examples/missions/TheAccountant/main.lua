-- The Accountant (C1-1): the Don's accountant paces the road by the garage,
-- going over the books. He knows too much. Take him out with your pistols,
-- then get back to the hill where you start.
--
-- Made in the editor: the accountant (NPCs tab, no behaviour), a point on the
-- hill, the target and the exit (Inspector > Mission: Make it a target, Make
-- it the exit). This script only walks him and tells the story.

local A = { 2641, 10, -5309 }   -- his round: two points of the path network
local B = { 4200, 10, -6200 }

h2.basemission(false)           -- C1-1's own targets do not end this mission
for slot = 1, 4 do              -- and its own laptop lines are not this mission's
  h2.objective(slot, false)
end
h2.briefing("The Don's accountant keeps the family's books in his head. Tonight he walks the road " ..
            "by the garage, going over the numbers. Make sure he never testifies, then leave " ..
            "the way you came: the hill.")

local accountant = h2.find("NPC_MafiaLawyer_1")
if not accountant then
  error("the accountant is not on the map")
end

h2.state.leg = h2.state.leg or 1
local waiting = false  -- reading at the end of the road
local function walk()
  if not accountant:alive() then return end
  local p = h2.state.leg == 1 and A or B
  accountant:walkto(p[1], p[2], p[3])
end

if accountant:alive() then
  accountant:moveset("Walk")
  h2.after(1, walk)
  -- at the end of the road he stops, reads, and turns back
  h2.every(1, function(t)
    if not accountant:alive() then t:cancel() return end
    local x, _, z = accountant:pos()
    local p = h2.state.leg == 1 and A or B
    if not waiting and (x - p[1]) ^ 2 + (z - p[3]) ^ 2 < 120 ^ 2 then
      waiting = true
      h2.state.leg = 3 - h2.state.leg
      h2.after(4, function() waiting = false walk() end)
    end
  end)
  h2.subtitle("The accountant walks the road by the garage.", 6)
  h2.after(7, function()
    h2.info("Down the hill, the road leads to the garage.", "hint")
  end)
end

h2.on("death", function(who)
  if who:path() == accountant:path() then
    h2.subtitle("The books are closed. Back to the hill.", 6)
  end
end)
