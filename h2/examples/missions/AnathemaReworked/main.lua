-- Anathema Reworked (C1-1 as a new mission): the map's own story is off; six
-- objectives of different kinds, all on the laptop:
--   1 take the Don's ledger (an item: pickup)
--   2 get into a bodyguard's suit (a disguise)
--   3 hurt no innocent (a rule: failing it fails the mission)
--   4 eight minutes (a time limit)
--   5 eliminate the new consigliere (editor goal: Make it a target)
--   6 check the garage (editor goal: Hitman has to get here)
-- then back up the hill. Hitman starts as the postman (Level tab > Hitman
-- starts in).

local TIME_LIMIT = 480
local EXIT = { 7285, 974, -7584 }

h2.basemission(false)
h2.briefing("The Don has a new consigliere and a new ledger. You come in as the postman. Take " ..
            "the ledger, get into a bodyguard's suit, check the garage for the Don's car and " ..
            "eliminate the consigliere. Hurt no innocent. Eight minutes, then back to the hill.")

h2.state.ledger = h2.state.ledger or false
h2.state.suit = h2.state.suit or false
h2.state.done5 = h2.state.done5 or false
h2.state.done6 = h2.state.done6 or false

local over = false
local function lines()
  h2.objective(1, "Take the Don's ledger", h2.state.ledger)
  h2.objective(2, "Get into a bodyguard's suit", h2.state.suit)
  h2.objective(3, "Hurt no innocent", over and not h2.state.failed)
  h2.objective(4, "Be out in eight minutes", over and not h2.state.failed)
end
lines()

local function finish(ok, text)
  if over then return end
  over = true
  h2.state.failed = not ok
  lines()
  h2.subtitle(text, 6)
  h2.after(3, function() h2.mission(ok and "complete" or "fail") end)
end

local function allDone()
  return h2.state.ledger and h2.state.suit and h2.state.done5 and h2.state.done6
end

-- 1: the ledger (the level's KeyRoom by the road). findall finds the item's
-- model too (".../KeyRoom#1/KeyRoom"): the item itself is the ZItem.
local ledger
for _, o in ipairs(h2.findall("KeyRoom")) do
  local x, _, z = o:pos()
  if o:isa("ZItem") and (x - 4200) ^ 2 + (z + 6200) ^ 2 < 300 ^ 2 then ledger = o end
end
h2.on("pickup", function(e)
  if not h2.state.ledger and ledger and e.object and e.object:path() == ledger:path() then
    h2.state.ledger = true
    lines()
    h2.info("The ledger. " .. (allDone() and "Back to the hill." or "Now the rest."))
  end
end)

-- 2: a bodyguard's suit (the start outfit's own change comes first: not counted)
local started = h2.time()
h2.on("disguise", function(e)
  if h2.time() - started < 3 or h2.state.suit then return end
  h2.state.suit = true
  lines()
  h2.info("The suit fits.")
end)

-- 3: no innocent
h2.on("kill", function(e)
  if e.kind == "innocent" then finish(false, "An innocent died. The contract is void.") end
end)
h2.on("harm", function(e)
  if e.kind == "innocent" then finish(false, "You hurt an innocent. The contract is void.") end
end)

-- 4: the clock (kept with saved games)
local deadline = h2.time() + (h2.state.left or TIME_LIMIT)
h2.on("save", function() h2.state.left = math.max(0, deadline - h2.time()) end)
h2.every(1, function(t)
  if over then t:cancel() return end
  if h2.time() > deadline then
    t:cancel()
    finish(false, "Eight minutes are up.")
  end
end)

-- 5, 6: the editor's goals report themselves
h2.on("objective", function(slot)
  if slot == 5 then h2.state.done5 = true end
  if slot == 6 then h2.state.done6 = true end
  if allDone() then h2.info("Everything is done: back to the hill.") end
end)

-- the way out
h2.zone{ x = EXIT[1], y = EXIT[2], z = EXIT[3], radius = 400,
  on_enter = function()
    if over then return end
    if allDone() then
      finish(true, "Job done.")
    else
      h2.info("Not everything is done yet.", "hint")
    end
  end }

h2.subtitle("You are the postman today.", 5)
