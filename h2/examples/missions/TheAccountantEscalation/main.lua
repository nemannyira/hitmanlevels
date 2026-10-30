-- The Accountant: Escalation (C1-1): The Accountant in three stages.
--   1 The Books: the accountant walks the road by the garage; take him out, back to the hill.
--   2 Quietly: he has a bodyguard (made here for stage 2 on: h2.stage()); never be seen.
--   3 On the Clock: five minutes; no innocents killed (only noted).
-- The stages, their texts and rules are the level's (Mission tab > Stages); this script walks
-- the accountant, tells the story and makes the bodyguard.

local A = { 2641, 10, -5309 }   -- his round: two points of the path network
local B = { 4200, 10, -6200 }

h2.basemission(false)           -- C1-1's own targets do not end this mission
for slot = 1, 4 do              -- and its own laptop lines are not this mission's
  h2.objective(slot, false)
end
h2.briefing("The Don's accountant keeps the family's books in his head. Tonight he walks the road " ..
            "by the garage, going over the numbers.")  -- (the stage's name, text and rules come before it)

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
  h2.after(11, function() h2.subtitle("The accountant walks the road by the garage.", 6) end)
  h2.after(18, function()
    h2.info("Down the hill, the road leads to the garage.", "hint")
  end)
end

h2.on("death", function(who)
  if who:path() == accountant:path() then
    h2.subtitle("The books are closed. Back to the hill.", 6)
  end
end)

-- stage 2 on: a bodyguard walks with him (BaseGuard: C1-1's guards' behaviour)
local stage = h2.stage()
if stage >= 2 and accountant:alive() then
  local guard = h2.npc("BodyguardMafia_01", A[1] + 150, A[2], A[3] + 100, 0, "BaseGuard")
  if guard then
    h2.every(2, function(t)
      if not guard:alive() or not accountant:alive() then t:cancel() return end
      local x, y, z = accountant:pos()
      guard:walkto(x + 150, y, z + 100)
    end)
  end
end
