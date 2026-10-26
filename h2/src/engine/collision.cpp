#include "collision.h"

#include <windows.h>
#include <cstring>

#include "../game.h"
#include "../log.h"

namespace h2::engine {

namespace {

constexpr int kManager_LineHitObject = 0x64 / 4;  // FUN_10027030 (result, node, from, dir, flag, mask)

using LineHit_t = bool(__thiscall*)(void* manager, float* result, ZBaseGeom* node, const float* from,
                                    const float* dir, char flag, uint32_t mask);

bool Call(void* manager, float* result, ZBaseGeom* node, const float* from, const float* dir, uint32_t mask)
{
    __try {
        auto fn = (*reinterpret_cast<LineHit_t**>(manager))[kManager_LineHitObject];
        return fn(manager, result, node, from, dir, 0, mask);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool CollisionHit(ZGEOM* geom, const float from[3], const float to[3], uint32_t mask, float hit[3])
{
    if (!geom || !IsReadable(geom, sizeof(ZGEOM)) || !geom->node || !IsReadable(geom->node, 0x70))
        return false;
    auto sys = reinterpret_cast<char*>(game::SysInterface());
    char* renderer = sys && IsReadable(sys + 0x18A0, 4) ? *reinterpret_cast<char**>(sys + 0x18A0) : nullptr;
    void* manager = renderer && IsReadable(renderer + 0x1C, 4) ? *reinterpret_cast<void**>(renderer + 0x1C) : nullptr;
    if (!manager)
        return false;
    float result[64] = {};  // (the collector fills up to +0x48)
    result[0] = 1.0f;       // the farthest hit taken: the whole segment
    float dir[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    if (!Call(manager, result, geom->node, from, dir, mask))
        return false;
    memcpy(hit, result + 1, 3 * sizeof(float));
    return true;
}

} // namespace h2::engine
