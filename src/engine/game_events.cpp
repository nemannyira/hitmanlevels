#include "game_events.h"

#include <windows.h>
#include <cctype>
#include <cstring>
#include <map>
#include <unordered_map>

#include "../game.h"
#include "../hook.h"
#include "../scene.h"
#include "messages.h"
#include "scene_data.h"

namespace h2::engine {

namespace {

constexpr uintptr_t kGeom_SendMessage = 0x43CBE0;  // ZGEOM vfunc 0x120 (this = sender; receiver, msg, param*)
constexpr uintptr_t kHitman_Damage = 0x519390;     // ZHITMAN2 (float amount)
constexpr uintptr_t kDragStart = 0x50A750;         // ZTRACKLNKHM2 vfunc 0x658 (dragger ref)
constexpr uintptr_t kDragEnd = 0x50A820;           // vfunc 0x65C ()
constexpr uint32_t kDragger = 0x762;               // the dragger's ref while dragged
constexpr uintptr_t kScript_Deliver = 0x5E13A0;    // script engine: (instance, name, param*) to a script, by name
constexpr uint32_t kMsgNames = 0xC37;              // engine: char* name[id]
constexpr uint32_t kMsgCount = 0x1C43;             // engine: the last id
constexpr uint32_t kMaxMessages = 0x402;           // room of the name table
constexpr uint32_t kHitmanHealth = 0xD72;          // float
constexpr size_t kMaxQueue = 4096;
constexpr int kLevel_Kill = 0xB4 / 4;              // (victim, kind)
constexpr int kLevel_Harm = 0xB8 / 4;
constexpr int kLevel_Failed = 0xC0 / 4;            // ()
constexpr int kLevel_Completed = 0xC4 / 4;

uint8_t g_flags[kMaxMessages];          // messages to keep (read by the hook)
bool g_any;                             // any flag set
std::string g_names[kMaxMessages];      // as registered
std::set<std::string> g_watch;          // lower case
bool g_watchAll;
uint32_t g_mappedCount = ~0u;           // message count the flags were made for
std::vector<GameEvent> g_queue;
size_t g_dropped;
// the message SendMessage is delivering (its script gets it again by name)
const char* g_sending[16];
int g_sendDepth;

void* Engine()
{
    auto sys = reinterpret_cast<char*>(game::SysInterface());
    return sys ? *reinterpret_cast<void**>(sys + 0x98) : nullptr;
}

bool CopyMem(void* dst, const void* src, size_t n)
{
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadDword(uintptr_t p, uint32_t& out)
{
    return p >= 0x10000 && CopyMem(&out, reinterpret_cast<const void*>(p), 4);
}

// the dwords a message's param points to (as many as are readable, up to 4)
void ReadWords(GameEvent& e)
{
    e.wordCount = 0;
    for (int i = 0; i < 4; ++i) {
        if (!ReadDword(e.param + i * 4, e.words[i]))
            break;
        e.wordCount = i + 1;
    }
    e.hasValue = e.wordCount > 0;
    e.value = e.words[0];
}

// a short printable NUL-terminated text at p (names, event names)
bool ReadText(uintptr_t p, char (&out)[65])
{
    if (p < 0x10000)
        return false;
    for (int i = 0; i < 64; ++i) {
        char c;
        if (!CopyMem(&c, reinterpret_cast<const char*>(p) + i, 1))
            return false;
        out[i] = c;
        if (!c)
            return i >= 2;
        if (c < 0x20 || c > 0x7E)
            return false;
    }
    return false;
}

std::string Lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

uint32_t MessageCount(void* engine)
{
    uint32_t n = 0;
    if (!engine || !CopyMem(&n, static_cast<char*>(engine) + kMsgCount, 4))
        return 0;
    return n < kMaxMessages ? n : kMaxMessages - 1;
}

std::string MessageName(void* engine, uint32_t id)
{
    uint32_t p = 0;
    if (!CopyMem(&p, static_cast<char*>(engine) + kMsgNames + id * 4, 4) || !p)
        return {};
    std::string s;
    for (int i = 0; i < 64; ++i) {
        char c;
        if (!CopyMem(&c, reinterpret_cast<const char*>(uintptr_t(p)) + i, 1) || !c)
            break;
        s += c;
    }
    return s;
}

// the flags for the names asked for, when the engine's messages changed
void Remap(bool force)
{
    void* engine = Engine();
    uint32_t count = MessageCount(engine);
    if (!force && count == g_mappedCount)
        return;
    g_mappedCount = count;
    memset(g_flags, 0, sizeof(g_flags));
    g_any = false;
    for (uint32_t id = 1; id <= count; ++id) {
        g_names[id] = MessageName(engine, id);
        if (g_names[id].empty())
            continue;
        if (g_watchAll || g_watch.count(Lower(g_names[id]))) {
            g_flags[id] = 1;
            g_any = true;
        }
    }
}

void Queue(GameEvent&& e)
{
    if (g_queue.size() >= kMaxQueue) {
        ++g_dropped;
        return;
    }
    g_queue.push_back(std::move(e));
}

using Send_t = void(__fastcall*)(ZGEOM* self, void* edx, ZGEOM* to, uint32_t msg, void* param);
Send_t oSend;
void __fastcall hkSend(ZGEOM* self, void* edx, ZGEOM* to, uint32_t msg, void* param)
{
    uint32_t id = msg & 0xFFFF;
    if (g_any && id < kMaxMessages && g_flags[id]) {
        GameEvent e;
        e.name = g_names[id];
        e.to = to;
        e.from = self;
        e.param = reinterpret_cast<uintptr_t>(param);
        ReadWords(e);
        char text[65];
        if (ReadText(e.param, text) || (e.hasValue && ReadText(e.value, text)))
            e.text = text;
        Queue(std::move(e));
    }
    if (g_sendDepth < 16)
        g_sending[g_sendDepth] = id < kMaxMessages ? g_names[id].c_str() : "";
    ++g_sendDepth;
    oSend(self, edx, to, msg, param);
    --g_sendDepth;
}

// a script's message name, lower case (false: unreadable or too long)
bool LowerName(const char* name, char (&out)[64])
{
    __try {
        for (int i = 0; i < 63; ++i) {
            char c = name[i];
            out[i] = c >= 'A' && c <= 'Z' ? char(c + 32) : c;
            if (!c)
                return i > 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

bool ReadPointer(const void* p, const void*& out)
{
    return CopyMem(&out, p, 4);
}

// Messages scripts get by name: from SendMessage (already kept: skipped) and
// from other scripts (the alarm, Hitman spotted...).
using Deliver_t = void(__fastcall*)(void* self, void* edx, void* instance, const char* name, void* param);
Deliver_t oDeliver;
void __fastcall hkDeliver(void* self, void* edx, void* instance, const char* name, void* param)
{
    if (g_watchAll || !g_watch.empty()) {
        char lower[64];
        const char* real = name;
        uintptr_t realParam = reinterpret_cast<uintptr_t>(param);
        if (LowerName(name, lower) && !strcmp(lower, "scriptevent")) {
            const void* inner = nullptr;  // {char* name, params...}
            if (param && ReadPointer(param, inner)) {
                real = static_cast<const char*>(inner);
                realParam += 4;
            }
        }
        const char* top = g_sendDepth > 0 && g_sendDepth <= 16 ? g_sending[g_sendDepth - 1] : nullptr;
        char topLower[64];
        bool fromSend = top && LowerName(top, topLower) &&
                        (!strcmp(topLower, "scriptevent") || (LowerName(name, lower) && !strcmp(topLower, lower)));
        if (!fromSend && LowerName(real, lower) && (g_watchAll || g_watch.count(lower))) {
            GameEvent e;
            e.name = real;
            e.instance = instance;
            e.param = realParam;
            ReadWords(e);
            char text[65];
            if (e.hasValue && ReadText(e.value, text))
                e.text = text;
            e.script = true;
            Queue(std::move(e));
        }
    }
    oDeliver(self, edx, instance, name, param);
}

using Damage_t = void(__fastcall*)(void* hitman, void* edx, float amount);
Damage_t oDamage;
void __fastcall hkDamage(void* hitman, void* edx, float amount)
{
    oDamage(hitman, edx, amount);
    GameEvent e;
    e.kind = GameEvent::HitmanDamage;
    e.amount = amount;
    CopyMem(&e.health, static_cast<char*>(hitman) + kHitmanHealth, 4);
    Queue(std::move(e));
}

using DragStart_t = void(__fastcall*)(void* body, void* edx, uint32_t dragger);
DragStart_t oDragStart;
void __fastcall hkDragStart(void* body, void* edx, uint32_t dragger)
{
    oDragStart(body, edx, dragger);
    GameEvent e;
    e.kind = GameEvent::Drag;
    e.to = static_cast<ZGEOM*>(body);
    e.hasValue = true;
    e.value = dragger;
    e.type = 1;
    Queue(std::move(e));
}

using DragEnd_t = void(__fastcall*)(void* body, void* edx);
DragEnd_t oDragEnd;
void __fastcall hkDragEnd(void* body, void* edx)
{
    GameEvent e;
    e.kind = GameEvent::Drag;
    e.to = static_cast<ZGEOM*>(body);
    e.hasValue = CopyMem(&e.value, static_cast<char*>(body) + kDragger, 4);
    e.type = 0;
    oDragEnd(body, edx);
    Queue(std::move(e));
}

// The level routine's entries, per vtable (each level has its own class).
struct LevelEntries {
    void* kill = nullptr;
    void* harm = nullptr;
    void* failed = nullptr;
    void* completed = nullptr;
};
std::map<void**, LevelEntries> g_levels;

const LevelEntries* EntriesOf(void* level)
{
    auto it = g_levels.find(*static_cast<void***>(level));
    return it == g_levels.end() ? nullptr : &it->second;
}

using Victim_t = void(__fastcall*)(void* level, void* edx, uint32_t victim, int type);
using End_t = void(__fastcall*)(void* level, void* edx);

// The characters the level routine was told of (the game's own: Being2 sends CharacterKilled /
// CharacterHarmed to LevelControl, 0x504960 -> Kill / Harm): for the editor's goals and its "death"
// event, not polled. Kept until the scene is left.
std::set<ZGEOM*> g_killed, g_harmed;
std::vector<ZGEOM*> g_newlyKilled;

ZGEOM* VictimGeom(uint32_t value)
{
    if (value >= 0x10000) {  // a pointer (as scripts' events pass it)
        void* vt = nullptr;
        if (CopyMem(&vt, reinterpret_cast<void*>(uintptr_t(value)), 4) && uintptr_t(vt) >= 0x600000 &&
            uintptr_t(vt) < 0x700000)
            return reinterpret_cast<ZGEOM*>(uintptr_t(value));
    }
    // a geom reference: dynamic ones are looked up in a table, static ones are offsets of 0x70 byte nodes
    bool ref = value && !(value & 0x80000000) && (!(value & 0x40000000) || (value & 0x3FFFFFFF) % 0x70 == 0);
    return ref ? GeomOfRef(value) : nullptr;
}

void QueueVictim(GameEvent::Kind kind, void* level, uint32_t victim, int type)
{
    if (ZGEOM* g = VictimGeom(victim)) {
        if (kind == GameEvent::Kill) {
            if (g_killed.insert(g).second && g_newlyKilled.size() < 1024)
                g_newlyKilled.push_back(g);
        } else {
            g_harmed.insert(g);
        }
    }
    GameEvent e;
    e.kind = kind;
    e.to = static_cast<ZGEOM*>(level);
    e.hasValue = true;
    e.value = victim;
    e.type = type;
    Queue(std::move(e));
}

void __fastcall hkKill(void* level, void* edx, uint32_t victim, int type)
{
    QueueVictim(GameEvent::Kill, level, victim, type);
    if (const LevelEntries* o = EntriesOf(level))
        reinterpret_cast<Victim_t>(o->kill)(level, edx, victim, type);
}

void __fastcall hkHarm(void* level, void* edx, uint32_t victim, int type)
{
    QueueVictim(GameEvent::Harm, level, victim, type);
    if (const LevelEntries* o = EntriesOf(level))
        reinterpret_cast<Victim_t>(o->harm)(level, edx, victim, type);
}

bool g_endsAllowed = true;  // SetLevelEndsAllowed
bool g_allowNext;           // AllowLevelEnd

// may this end of the mission go through? (one AllowLevelEnd lets one through)
bool EndGoesThrough(bool failed)
{
    bool editors = g_allowNext;
    g_allowNext = false;
    if (g_endsAllowed || editors)
        return true;
    ZGEOM* hitman = Player();
    return failed && hitman && !IsAlive(hitman);  // Hitman's death ends it as always
}

void __fastcall hkFailed(void* level, void* edx)
{
    GameEvent e;
    e.kind = GameEvent::MissionFailed;
    bool through = EndGoesThrough(true);
    e.type = through ? 0 : 1;
    Queue(std::move(e));
    if (!through) {
        Log("game events: the base level's mission failure stopped");
        return;
    }
    if (const LevelEntries* o = EntriesOf(level))
        reinterpret_cast<End_t>(o->failed)(level, edx);
}

void __fastcall hkCompleted(void* level, void* edx)
{
    GameEvent e;
    e.kind = GameEvent::MissionCompleted;
    bool through = EndGoesThrough(false);
    e.type = through ? 0 : 1;
    Queue(std::move(e));
    if (!through) {
        Log("game events: the base level's mission completion stopped");
        return;
    }
    if (const LevelEntries* o = EntriesOf(level))
        reinterpret_cast<End_t>(o->completed)(level, edx);
}

bool PatchEntry(void** vt, int index, void* detour)
{
    DWORD old;
    if (!VirtualProtect(&vt[index], sizeof(void*), PAGE_READWRITE, &old))
        return false;
    vt[index] = detour;
    VirtualProtect(&vt[index], sizeof(void*), old, &old);
    return true;
}

// the script instance of a geom: extra data (+0x10) -> script list (+0x1C)
// -> entry (+4) -> instance (+4), as HandleMessage 0x43CFE0 reads it
void* InstanceOf(const ZGEOM* g)
{
    __try {
        const char* extra = *reinterpret_cast<char* const*>(reinterpret_cast<const char*>(g) + 0x10);
        if (!extra)
            return nullptr;
        const char* list = *reinterpret_cast<char* const*>(extra + 0x1C);
        if (!list)
            return nullptr;
        const char* entry = *reinterpret_cast<char* const*>(list + 4);
        return entry ? *reinterpret_cast<void* const*>(entry + 4) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

} // namespace

ZGEOM* ScriptGeom(void* instance, const SceneSnapshot& scene)
{
    static const SceneSnapshot* cached;
    static const GeomInfo* data;
    static size_t size;
    static std::unordered_map<void*, ZGEOM*> geoms;
    if (&scene != cached || scene.geoms.data() != data || scene.geoms.size() != size) {
        geoms.clear();
        for (const GeomInfo& g : scene.geoms)
            if (void* i = g.geom ? InstanceOf(g.geom) : nullptr)
                geoms.emplace(i, g.geom);
        cached = &scene;
        data = scene.geoms.data();
        size = scene.geoms.size();
    }
    auto it = geoms.find(instance);
    return it == geoms.end() ? nullptr : it->second;
}

void InstallGameEvents()
{
    Hook("ZGEOM::SendMessage", kGeom_SendMessage, hkSend, &oSend);
    Hook("ZHITMAN2::Damage", kHitman_Damage, hkDamage, &oDamage);
    Hook("Script::Deliver", kScript_Deliver, hkDeliver, &oDeliver);
    Hook("ZTRACKLNKHM2::DragStart", kDragStart, hkDragStart, &oDragStart);
    Hook("ZTRACKLNKHM2::DragEnd", kDragEnd, hkDragEnd, &oDragEnd);
    OnSceneLeaving([](const std::string&) {
        g_endsAllowed = true;
        g_allowNext = false;
        g_queue.clear();
        g_any = false;  // ids may be given again with the next scene
        g_mappedCount = ~0u;
    });
    OnScenePushed([](const std::string&) { Remap(true); });
}

void SetLevelEndsAllowed(bool allowed) { g_endsAllowed = allowed; }
bool LevelEndsAllowed() { return g_endsAllowed; }
void AllowLevelEnd() { g_allowNext = true; }

void WatchMessages(const std::set<std::string>& names, bool all)
{
    if (names == g_watch && all == g_watchAll)
        return;
    g_watch = names;
    g_watchAll = all;
    Remap(true);
}

void WatchLevel(void* level)
{
    void** vt = nullptr;
    if (!level || !CopyMem(&vt, level, 4) || !vt || g_levels.count(vt))
        return;
    LevelEntries o;
    if (!CopyMem(&o.kill, &vt[kLevel_Kill], 4) || !CopyMem(&o.harm, &vt[kLevel_Harm], 4) ||
        !CopyMem(&o.failed, &vt[kLevel_Failed], 4) || !CopyMem(&o.completed, &vt[kLevel_Completed], 4))
        return;
    g_levels[vt] = o;  // before the entries change: a call finds its original
    bool ok = PatchEntry(vt, kLevel_Kill, reinterpret_cast<void*>(hkKill)) &&
              PatchEntry(vt, kLevel_Harm, reinterpret_cast<void*>(hkHarm)) &&
              PatchEntry(vt, kLevel_Failed, reinterpret_cast<void*>(hkFailed)) &&
              PatchEntry(vt, kLevel_Completed, reinterpret_cast<void*>(hkCompleted));
    Log("game events: level routine vtable %p %s", static_cast<void*>(vt), ok ? "watched" : "not writable");
}

bool WasKilled(ZGEOM* character) { return character && g_killed.count(character); }
bool WasHarmed(ZGEOM* character) { return character && g_harmed.count(character); }

std::vector<ZGEOM*> TakeKilled()
{
    std::vector<ZGEOM*> out;
    out.swap(g_newlyKilled);
    return out;
}

std::vector<GameEvent> TakeGameEvents()
{
    if (!SceneDataValid()) {  // between scenes: the objects are going
        g_killed.clear();
        g_harmed.clear();
        g_newlyKilled.clear();
        g_queue.clear();
        return {};
    }
    if (g_watchAll || !g_watch.empty())
        Remap(false);  // scripts' classes register their messages while they load
    if (g_dropped) {
        Log("game events: %zu events of one frame dropped (more than %zu)", g_dropped, kMaxQueue);
        g_dropped = 0;
    }
    std::vector<GameEvent> out;
    out.swap(g_queue);
    return out;
}

std::vector<std::string> MessageNames()
{
    std::vector<std::string> out;
    void* engine = Engine();
    uint32_t count = MessageCount(engine);
    for (uint32_t id = 1; id <= count; ++id) {
        std::string n = MessageName(engine, id);
        if (!n.empty())
            out.push_back(Lower(n));
    }
    return out;
}

} // namespace h2::engine
