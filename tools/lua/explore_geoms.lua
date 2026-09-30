-- Reverse engineering helper: dumps the engine's geom table, nodes and class
-- infos to H2Fix.log once a playable scene ("...__MAIN") has run for a while.
-- Needs [Debug] LuaMemory=1. Copy to <game>\Mods\autoexec.lua.
local m = h2.mem
local function hx(v) return v and string.format("%08X", v) or "nil" end

local function strAt(p)
  if not p or p < 0x10000 then return nil end
  local s = m.str(p, 64)
  if s and #s >= 3 and not s:find("[^%g ]") then return s end
  return nil
end

local function dumpObject(label, p, len)
  h2.log(("%s %s:"):format(label, hx(p)))
  for off = 0, len - 4, 4 do
    local v = m.u32(p + off)
    local s = v and strAt(v)
    h2.log(("  +%03X %s %s"):format(off, hx(v), s and ("'" .. s .. "'") or ""))
  end
end

local function explore()
  local sys = m.u32(0x6A6C5C)
  local engine = m.u32(sys + 0x98)
  local tbl = m.u32(engine + 4)
  h2.log(("engine %s geomtable %s root(+AD8) %s"):format(hx(engine), hx(tbl), hx(m.u32(engine + 0xAD8))))
  dumpObject("geomtable", tbl, 0x40)
  local vt = m.u32(tbl)
  for off = 0, 0x70, 4 do h2.log(("  vt+%02X %s"):format(off, hx(m.u32(vt + off)))) end

  local base = m.vcall(tbl, 0x4C)
  h2.log("static node base " .. hx(base))
  local bad = 0
  for i = 0, 4000 do
    local node = base + i * 0x70
    local geom = m.u32(node + 0x54)
    local gv = geom and m.u32(geom)
    if not gv or gv < 0x401000 or gv > 0x6DC000 then
      bad = bad + 1
      if bad > 30 then h2.log(("stop at index %d"):format(i)); break end
    else
      bad = 0
      if i < 60 or i % 200 == 0 then
        local ci = m.u32(geom + 0xC)
        h2.log(("node[%d] %s geom %s vt %s parent %s child %d next %d flags %s ci %s id %s pos %.1f %.1f %.1f"):format(
          i, hx(node), hx(geom), hx(gv), hx(m.u32(node + 0x50)), m.u16(node + 0x5C), m.u16(node + 0x5E),
          hx(m.u32(node + 0x3C)), hx(ci), hx(ci and m.u32(ci + 0x23)),
          m.f32(node + 0x24), m.f32(node + 0x28), m.f32(node + 0x2C)))
      end
    end
  end

  -- class info chain
  local ci = m.u32(0x6A6C64)
  for n = 1, 6 do
    if not ci or ci == 0 then break end
    dumpObject("classinfo#" .. n, ci, 0x40)
    ci = m.u32(ci + 0x14)
  end
  dumpObject("ZCAMERA_ClassInfo", 0x6A6DA4, 0x50)
  -- first geom of the table: dump its object layout
  local g = m.u32(base + 0x54)
  if g then dumpObject("geom0", g, 0x60) end
end

local frames, armed = 0, false
h2.on("scene", function(name)
  armed = name:find("__MAIN") ~= nil
  frames = 0
end)
h2.on("frame", function()
  if not armed then return end
  frames = frames + 1
  if frames == 120 then
    armed = false
    local ok, err = pcall(explore)
    if not ok then h2.log("explore failed: " .. tostring(err)) end
    h2.log("explore done")
  end
end)
print("explore_geoms armed")
