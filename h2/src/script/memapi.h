#pragma once

namespace h2::script {

// Debug-only raw memory access for reverse engineering from Lua
// ([Debug] LuaMemory=1). Not available to regular level scripts.
//   h2.mem.u8/u16/u32/i32/f32(addr)   -> value or nil on an unreadable address
//   h2.mem.str(addr [, max])          -> string or nil
//   h2.mem.hex(addr, len)             -> hex dump string
//   h2.mem.vcall(obj, byteOffset, ...) -> eax of a __thiscall virtual (up to 6 int args)
//   h2.mem.call(addr, this, ...)       -> eax of a __thiscall function (up to 6 int args)
void RegisterMemApi();

} // namespace h2::script
