#include "mainloop.h"

#include <vector>

#include "game.h"
#include "hook.h"

namespace h2 {

namespace {

std::vector<FrameCallback> g_frameEnd;

using MainLoop_t = void(__fastcall*)(void* self, void* edx, bool arg);
MainLoop_t oMainLoop;

void __fastcall hkMainLoop(void* self, void* edx, bool arg)
{
    oMainLoop(self, edx, arg);
    for (FrameCallback cb : g_frameEnd)
        cb();
}

} // namespace

void OnFrameEnd(FrameCallback cb)
{
    if (g_frameEnd.empty())
        Hook("ZEngineDataBase::MainLoop", game::ZEngineDataBase_MainLoop, hkMainLoop, &oMainLoop);
    g_frameEnd.push_back(cb);
}

} // namespace h2
