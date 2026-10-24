"""Issue #2: a character made with its behaviour and killed while that behaviour is still starting (map_smoke's
"kill early" on C5-3 took the game down twice: INT3 in the handle table's free 0x5B2B10, then in the script
VM 0x5E0260). Makes a scripted soldier of the map's own kind and kills it after `delay` seconds, `rounds`
times, on C5-3 (and C1-1 for comparison); the game must live and each kill must count.

  python tools/tests/kill_early_test.py [rounds]
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import dev  # noqa: E402

ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 12
LOG = os.path.join(dev.GAME, "H2Fix.log")
results = []

SETUP = r'''
local hx, hy, hz = h2.player():pos()
KE_SPOT = nil
for r = 0, 6000, 200 do
  for a = 0, 330, 30 do
    local x, z = hx + r * math.cos(math.rad(a)), hz + r * math.sin(math.rad(a))
    for dy = 200, -400, -50 do
      if h2.onpath(x, hy + dy, z) then KE_SPOT = {x, hy + dy, z} break end
    end
    if KE_SPOT then break end
  end
  if KE_SPOT then break end
end
if not KE_SPOT and KE_FALLBACK then KE_SPOT = KE_FALLBACK end  -- (C1-1: Hitman starts off the network)
KE_MODEL = nil
for _, n in ipairs(h2.npcs()) do if n.model and n.model ~= "" then KE_MODEL = n.model break end end
KE_KILLED, KE_MADE = 0, 0
return tostring(KE_MODEL) .. " " .. tostring(KE_SPOT ~= nil)
'''

ROUND = r'''
local g = h2.npc(KE_MODEL, KE_SPOT[1] + 60, KE_SPOT[2], KE_SPOT[3])
if not g then return "not made" end
KE_MADE = KE_MADE + 1
h2.after(%s, function() if g:kill() then KE_KILLED = KE_KILLED + 1 end end)
return "made"
'''


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def alive():
    return dev.running() and "timeout" not in dev.run("return 1", 8)


def log_size():
    return os.path.getsize(LOG) if os.path.exists(LOG) else 0


def log_since(at):
    with open(LOG, encoding="latin1") as f:
        f.seek(at)
        return f.read()


for scene, short, fallback in [("C5-3\\C5-3__MAIN", "C5-3", "nil"),
                               ("C1-1\\C1-1__MAIN", "C1-1", "{-4176, -20, -6682}")]:  # (the Courier example's spot)
    if not alive():
        check("%s: the game runs" % short, False)
        break
    dev.run("h2.dev_editor(false) return h2.dev_level('revert')", 10)
    dev.run("return h2.dev_loadscene([[%s]])" % scene, 10)
    if not dev.wait_ready(scene.split("\\")[1], 200):
        check("%s loaded" % short, False)
        continue
    time.sleep(3)
    mark = log_size()
    info = dev.run("KE_FALLBACK = %s\n" % fallback + SETUP, 20)
    if "nil" in info or "false" in info:
        check("%s: a soldier and a spot" % short, False, info)
        continue
    for delay in ("1", "0.1", "0"):
        for i in range(ROUNDS // 3):
            dev.run(ROUND % delay, 10)
            time.sleep(1.6)
            if not alive():
                break
        if not alive():
            break
    time.sleep(2)
    up = alive()
    out = log_since(mark)
    check("%s: the game lives after %d early kills" % (short, ROUNDS), up,
          [l.strip()[-90:] for l in out.splitlines() if "EXCEPTION" in l or "CRASH" in l][:3])
    if up:
        made_killed = dev.run("return KE_MADE .. ' ' .. KE_KILLED", 10).split()
        check("%s: every one made was killed" % short, len(made_killed) == 2 and made_killed[0] == made_killed[1]
              and made_killed[0] != "0", made_killed)
    check("%s: no engine assert (INT3) in the log" % short, "EXCEPTION 80000003" not in out,
          [l.strip()[-90:] for l in out.splitlines() if "80000003" in l][:2])

print("\n%d passed, %d failed" % (results.count(True), results.count(False)))
sys.exit(0 if all(results) else 1)
