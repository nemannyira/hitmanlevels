-- Arena (C1-1 with no buildings, on the yard of C3-3): three armed
-- bodyguards wait across an open yard.
-- Eliminate them and reach the far corner.
--
-- Made in the editor only, but for this text: New level with "No buildings
-- either" and the C3-3 stone yard (main menu > Level Editor), three guards
-- (NPCs tab: BaseGuard), a point in
-- the far corner; Make it a target (x3) and Make it the exit (Inspector >
-- Mission); the objectives' texts (Mission tab).

h2.briefing("An open yard with nowhere to hide. Three of the Don's bodyguards " ..
            "wait on the far side, armed. Eliminate all three, then leave by the far corner.")

local guards = {}
for i = 1, 3 do guards[i] = h2.find("NPC_BodyguardMafia_01_" .. i) end

local function left()
  local n = 0
  for _, g in ipairs(guards) do
    if g and g:alive() then n = n + 1 end
  end
  return n
end

h2.subtitle("Three bodyguards across the yard.", 6)

h2.on("death", function(who)
  for _, g in ipairs(guards) do
    if g and who:path() == g:path() then
      local n = left()
      if n > 0 then
        h2.info(n .. (n == 1 and " bodyguard left." or " bodyguards left."))
      else
        h2.subtitle("The yard is quiet. The far corner, now.", 6)
      end
    end
  end
end)
