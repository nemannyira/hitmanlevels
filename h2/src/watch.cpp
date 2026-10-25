#include "watch.h"

#include <windows.h>
#include <string>

#include "log.h"

namespace h2 {

namespace {

struct Slot {
    uintptr_t addr = 0;
    WatchCallback cb = nullptr;
    bool pending = false;  // loaded into DRx on the next handler entry
    int hits = 0;
    bool disarm = false;   // clear DRx enable on the next handler entry
    bool reads = false;    // break on reads too (R/W = 11)
};
constexpr int kMaxHits = 20;
Slot g_slots[4];

void Apply(CONTEXT* ctx)
{
    DWORD* dr[4] = {&ctx->Dr0, &ctx->Dr1, &ctx->Dr2, &ctx->Dr3};
    for (int i = 0; i < 4; ++i) {
        if (g_slots[i].disarm) {
            g_slots[i].disarm = false;
            ctx->Dr7 &= ~((3u << (i * 2)) | (0xFu << (16 + i * 4)));
        }
        if (!g_slots[i].pending)
            continue;
        g_slots[i].pending = false;
        *dr[i] = static_cast<DWORD>(g_slots[i].addr);
        ctx->Dr7 &= ~(0xFu << (16 + i * 4));
        ctx->Dr7 |= (1u << (i * 2))              // local enable
                    | ((g_slots[i].reads ? 0x3u : 0x1u) << (16 + i * 4))  // R/W: 01 write, 11 read/write
                    | (0x3u << (18 + i * 4));    // LEN = 11: 4 bytes
    }
    ctx->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
}

std::string Backtrace(const CONTEXT* ctx)
{
    std::string s;
    auto sp = reinterpret_cast<const DWORD*>(ctx->Esp);
    int found = 0;
    for (int i = 0; i < 2048 && found < 10; ++i) {
        DWORD v;
        if (IsBadReadPtr(sp + i, 4))
            break;
        v = sp[i];
        if (v >= 0x401000 && v < 0x639000) {
            char b[16];
            snprintf(b, sizeof(b), " %06lX", v);
            s += b;
            ++found;
        }
    }
    return s;
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep)
{
    CONTEXT* ctx = ep->ContextRecord;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT) {
        // our arming int3 (see WatchWrite): skip it and load the registers
        bool any = false;
        for (auto& s : g_slots)
            any |= s.pending;
        if (!any)
            return EXCEPTION_CONTINUE_SEARCH;
        ctx->Eip += 1;
        Apply(ctx);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    DWORD hit = ctx->Dr6 & 0xF;
    if (!hit)
        return EXCEPTION_CONTINUE_SEARCH;
    for (int i = 0; i < 4; ++i) {
        if (!(hit & (1u << i)))
            continue;
        // A read watch is still armed inside the handler: touching the address
        // here would re-enter it, so only write watches report the value.
        uint32_t value = g_slots[i].reads ? 0 : *reinterpret_cast<uint32_t*>(g_slots[i].addr);
        Log("watch[%d] %08X = %08X (%d) accessed by eip %08lX, stack:%s", i, unsigned(g_slots[i].addr), value,
            int(value), ctx->Eip, Backtrace(ctx).c_str());
        if (g_slots[i].cb)
            g_slots[i].cb(i, g_slots[i].addr, value);
        if (++g_slots[i].hits >= kMaxHits && !g_slots[i].pending) {
            Log("watch[%d]: %d hits, disarmed", i, kMaxHits);
            g_slots[i].disarm = true;
        }
    }
    ctx->Dr6 = 0;
    Apply(ctx);
    return EXCEPTION_CONTINUE_EXECUTION;
}

} // namespace

void WatchInit()
{
    AddVectoredExceptionHandler(1, Handler);
}

// From a callback the slot is loaded when the handler returns; from normal
// code call WatchArmNow() afterwards.
void WatchWrite(int slot, uintptr_t addr, WatchCallback cb)
{
    g_slots[slot] = {addr, cb, true, 0, false, false};
}

void WatchAccess(int slot, uintptr_t addr, WatchCallback cb)
{
    g_slots[slot] = {addr, cb, true, 0, false, true};
}

// Arm pending slots from normal code: raise an int3 that the handler consumes.
void WatchArmNow()
{
    __debugbreak();
}

} // namespace h2
