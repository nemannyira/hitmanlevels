-- Estate Run (C1-1, emptied map): a race against the clock around the Don's
-- estate. Four checkpoints in order, a lamp marks each; then back up the
-- hill. Two minutes and a half.
--
-- Made in the editor: Empty map (Level tab), four points (Models tab >
-- Points, or the NPCs tab's new point): "Point+1" .. "Point+4" in the order
-- they were placed (the Inspector shows each one's name). This script is the
-- race.

local CHECKPOINTS = {
  { point = "Point+1", name = "the road below the hill" },
  { point = "Point+2", name = "the garage" },
  { point = "Point+3", name = "the front gate" },
  { point = "Point+4", name = "the postman's corner" },
}
local FINISH = { 7285, 974, -7584, name = "the hill where you started" }
local TIME_LIMIT = 150
local MARKER = "Lamp_01"            -- a model of the map that marks the next checkpoint

h2.state.next = h2.state.next or 1  -- the checkpoint to reach (#CHECKPOINTS + 1: the finish)

h2.briefing("A race around the Don's estate: four checkpoints in order, then back up the hill " ..
            "where you start. A lamp marks the next one. Two minutes and a half.")

local function lines()
  local n = h2.state.next
  h2.objective(1, "Reach the four checkpoints in order (" .. math.min(n - 1, #CHECKPOINTS) .. "/4)",
               n > #CHECKPOINTS)
  h2.objective(2, "Then back up the hill", n > #CHECKPOINTS + 1)
end
lines()

-- where each checkpoint is
local spots = {}
for i, c in ipairs(CHECKPOINTS) do
  local p = h2.find(c.point)
  if not p then error("Estate Run: checkpoint " .. c.point .. " is not on the map") end
  local x, y, z = p:pos()
  spots[i] = { x, y, z, name = c.name }
end
spots[#spots + 1] = FINISH

-- the lamp over the next checkpoint
local source = h2.find(MARKER)
local marker
local function mark()
  if marker and marker:valid() then marker:destroy() end
  marker = nil
  local s = spots[h2.state.next]
  if s and source then
    marker = h2.spawn(source, s[1], s[2], s[3])
  end
end

local finished = false
local function finish(ok, text)
  if finished then return end
  finished = true
  if marker and marker:valid() then marker:destroy() end
  h2.subtitle(text, 6)
  h2.after(3, function() h2.mission(ok and "complete" or "fail") end)
end

-- the clock (kept with saved games); every 30 s the time left is told
local deadline = h2.time() + (h2.state.left or TIME_LIMIT)
h2.on("save", function() h2.state.left = math.max(0, deadline - h2.time()) end)
local told = math.floor(((h2.state.left or TIME_LIMIT) - 1) / 30)  -- the first word after 30 s
h2.every(1, function(t)
  if finished then t:cancel() return end
  local left = deadline - h2.time()
  if left <= 0 then
    t:cancel()
    finish(false, "Time is up.")
  elseif math.floor(left / 30) < told then
    told = math.floor(left / 30)
    h2.info(string.format("%d seconds left. Next: %s.", math.ceil(left), spots[h2.state.next].name),
            left < 30 and "warning" or "info")
  end
end)

-- each checkpoint: a zone that counts only when it is the next one
for i, s in ipairs(spots) do
  h2.zone{ x = s[1], y = s[2], z = s[3], radius = 300,
    on_enter = function()
      if finished or h2.state.next ~= i then return end
      h2.state.next = i + 1
      lines()
      if i == #spots then
        finish(true, string.format("Done, %d seconds to spare.", math.floor(deadline - h2.time())))
      else
        h2.subtitle(string.format("Checkpoint %d. Next: %s.", math.min(i, #CHECKPOINTS), spots[i + 1].name), 4)
        mark()
      end
    end }
end

mark()
h2.subtitle("Go! First: " .. spots[h2.state.next].name .. ".", 5)
