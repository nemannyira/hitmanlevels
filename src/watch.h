#pragma once
#include <cstdint>

namespace h2 {

// Debug aid: hardware write watchpoints on the main thread (DR0-DR3) with a
// vectored exception handler that logs the writing instruction and a
// heuristic backtrace. `chain` is called on each hit and may arm new
// watchpoints (e.g. a field of an object whose pointer was just written).
using WatchCallback = void (*)(int slot, uintptr_t addr, uint32_t value);

void WatchInit();
// Queue slot 0..3 for 4-byte writes at addr. Inside a WatchCallback it takes
// effect on return; elsewhere follow with WatchArmNow() on the thread to watch
// (an int3 round trip into the handler loads its debug registers).
void WatchWrite(int slot, uintptr_t addr, WatchCallback cb = nullptr);
void WatchArmNow();
// Same as WatchWrite but also breaks on reads (the logged eip is the
// instruction *after* the access).
void WatchAccess(int slot, uintptr_t addr, WatchCallback cb = nullptr);

} // namespace h2
