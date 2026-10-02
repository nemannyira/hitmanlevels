#include "messages.h"

#include <windows.h>
#include <cctype>
#include <set>
#include <string>

#include "../game.h"
#include "scene_data.h"

namespace h2::engine {

namespace {

constexpr uintptr_t kRegisterZMsg = 0x41EED0;  // ZEngineDataBase::RegisterZMsg(name, 0, file, line) -> ushort
constexpr int kGeom_SendMessage = 0x120 / 4;   // (ZGEOM* from, int msg, void* param)
constexpr int kActor_IsAlive = 0x544 / 4;
constexpr uint32_t kHitmanClass = 0x2003E8;
constexpr uint32_t kLevelControlClass = 0x2000E1;
const char* const kInstantDeath = "Msg_InstantDeath";

template <typename T>
T VFunc(void* obj, int index)
{
    return reinterpret_cast<T>((*reinterpret_cast<void***>(obj))[index]);
}

void* Engine()
{
    auto sys = reinterpret_cast<char*>(game::SysInterface());
    return sys ? *reinterpret_cast<void**>(sys + 0x98) : nullptr;
}

int SafeRegister(void* engine, const char* name)
{
    __try {
        return reinterpret_cast<uint16_t(__thiscall*)(void*, const char*, int, const char*, int)>(kRegisterZMsg)(
            engine, name, 0, "h2editor", 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool SafeSend(ZGEOM* to, ZGEOM* from, int msg, void* param)
{
    __try {
        VFunc<void(__thiscall*)(ZGEOM*, ZGEOM*, int, void*)>(to, kGeom_SendMessage)(to, from, msg, param);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int SafeIsAlive(ZGEOM* actor)
{
    __try {
        return VFunc<int(__thiscall*)(ZGEOM*)>(actor, kActor_IsAlive)(actor) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool Usable(ZGEOM* g)
{
    return g && IsReadable(g, sizeof(ZGEOM)) && g->node && IsReadable(g->node, sizeof(ZBaseGeom)) &&
           g->node->geom == g && SceneDataValid();
}

ZGEOM* FirstRecord(bool (*match)(const SceneRecord&))
{
    for (const SceneRecord& r : SceneRecords())
        if (match(r))
            if (ZGEOM* g = RecordGeom(r.index); Usable(g))
                return g;
    return nullptr;
}

} // namespace

uint16_t RegisterMessage(const char* name)
{
    // the engine keeps the name pointer: names live as long as the game
    static std::set<std::string> names;
    void* engine = Engine();
    if (!engine || !name)
        return 0;
    const std::string& kept = *names.insert(name).first;
    int id = SafeRegister(engine, kept.c_str());
    return id < 0 ? 0 : uint16_t(id);
}

bool SendMessage(ZGEOM* to, ZGEOM* from, uint16_t msg, void* param)
{
    if (!Usable(to) || !msg)
        return false;
    return SafeSend(to, from ? from : to, msg, param);
}

bool IsAlive(ZGEOM* actor)
{
    if (!Usable(actor) || !IsA(actor, "ZActor"))
        return false;
    return SafeIsAlive(actor) == 1;
}

bool KillActor(ZGEOM* actor)
{
    if (!IsAlive(actor))
        return false;
    static const char* param = kInstantDeath;
    return SendMessage(actor, actor, RegisterMessage("ScriptEvent"), &param);
}

ZGEOM* Player()
{
    return FirstRecord([](const SceneRecord& r) { return r.classId == kHitmanClass; });
}

ZGEOM* LevelControl()
{
    return FirstRecord([](const SceneRecord& r) {
        if (r.classId != kLevelControlClass || !r.script)
            return false;
        std::string n;
        for (char c : r.name)
            n += char(std::tolower(static_cast<unsigned char>(c)));
        return n.rfind("levelcontrol", 0) == 0;
    });
}

} // namespace h2::engine
