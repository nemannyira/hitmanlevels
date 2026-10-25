-- Courier (C1-1): a small mission made with the editor's Lua (h2/docs/lua.md).
--
-- A courier walks his round at the front gate with documents. Stop him,
-- take the documents and get back to the hill where you started. If he is
-- not stopped in three minutes, he leaves and the mission fails. The story
-- goes on from where it was after a saved game is loaded.

local START = { 7285, 974, -7584 }          -- Hitman's start: the way out
local ROUND = {                             -- points of the path network at the gate
  { -4176, -20, -6682 },
  { -4637, -20, -6353 },
  { -4076, -20, -6282 },
}
local TIME_LIMIT = 180                      -- game seconds
local DOCUMENTS = "KeyRoom"                 -- the item template that plays the documents

-- stages: 1 the courier walks, 2 the documents lie there, 3 taken, 4 over
h2.state.stage = h2.state.stage or 1
h2.state.leg = h2.state.leg or 1

local courier = h2.find("Courier")
if not courier then
  error("Courier: the level's character 'Courier' is not in the map")
end

h2.briefing("A courier of the Don's family carries papers that must never reach the mansion. " ..
            "He walks his round at the front gate. Stop him, take the papers and get out " ..
            "the way you came. Be quick: in three minutes he leaves.")

local function lines()
  local s = h2.state.stage
  h2.objective(1, "Stop the courier at the front gate", s >= 2)
  h2.objective(2, "Take the papers he carries", s >= 3)
  h2.objective(3, "Get back to the hill where you started", s >= 4 and not h2.state.failed)
end
lines()
h2.objective(4, false)  -- C1-1's own fourth line

if h2.state.stage == 1 then
  h2.subtitle("The courier walks his round at the front gate.", 6)
  h2.after(8, function()
    h2.info("The courier keeps the papers on him: they fall where he does.", "hint")
  end)
end

-- the round: on to the next point when he is there; again when he stands
local lastPos, still = nil, 0
local function walk()
  local p = ROUND[h2.state.leg]
  courier:walkto(p[1], p[2], p[3])
end

if h2.state.stage == 1 then
  courier:moveset("Walk")
  h2.after(2, walk)
  h2.every(1, function(t)
    if h2.state.stage ~= 1 or not courier:alive() then
      t:cancel()
      return
    end
    local x, _, z = courier:pos()
    local p = ROUND[h2.state.leg]
    if (x - p[1]) ^ 2 + (z - p[3]) ^ 2 < 100 ^ 2 then
      h2.state.leg = h2.state.leg % #ROUND + 1
      courier:lookat(h2.find("BodyguardMafia_FrontGate_00"))
      h2.after(2, walk)
      still = 0
    elseif lastPos and (x - lastPos[1]) ^ 2 + (z - lastPos[2]) ^ 2 < 10 ^ 2 then
      still = still + 1
      if still >= 5 then  -- stuck or stopped: on again
        still = 0
        walk()
      end
    else
      still = 0
    end
    lastPos = { x, z }
  end)
end

-- the time limit, kept across saved games
local deadline = h2.time() + (h2.state.left or TIME_LIMIT)
h2.on("save", function()
  h2.state.left = math.max(0, deadline - h2.time())
end)
local warned = false
h2.every(1, function(t)
  if h2.state.stage ~= 1 then
    t:cancel()
  elseif not warned and deadline - h2.time() < 60 then
    warned = true
    h2.subtitle("One minute: the courier is about to leave.", 5)
  elseif h2.time() > deadline then
    t:cancel()
    h2.state.stage, h2.state.failed = 4, true
    h2.subtitle("The courier is gone with the papers.", 5)
    courier:moveset("Run")
    courier:walkto(ROUND[2][1], ROUND[2][2], ROUND[2][3])
    h2.mission("fail")
  end
end)

-- the papers: he carries them (they fall by his body when he dies); after a
-- saved game they are put back where they fell
local papers
if h2.state.stage == 1 then
  courier:carry(DOCUMENTS, function(item)
    papers = item
    h2.state.papers = { item:pos() }
  end)
elseif h2.state.stage == 2 and h2.state.papers then
  local p = h2.state.papers
  papers = h2.item(DOCUMENTS, p[1], p[2], p[3])
end

h2.on("death", function(who)
  if h2.state.stage == 1 and who:path() == courier:path() then
    h2.state.stage = 2
    lines()
    h2.subtitle("The papers fell where he lies.", 5)
  end
end)

h2.on("pickup", function(e)
  if h2.state.stage == 2 and papers and e.object and e.object:path() == papers:path() then
    h2.state.stage = 3
    lines()
    h2.subtitle("Got them. Back to the hill.", 5)
  end
end)

-- the way out
h2.zone{ x = START[1], y = START[2], z = START[3], radius = 400,
  on_enter = function()
    if h2.state.stage == 3 then
      h2.state.stage = 4
      lines()
      h2.after(2, function() h2.mission("complete") end)
    end
  end }
