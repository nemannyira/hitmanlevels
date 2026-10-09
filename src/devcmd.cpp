#include "devcmd.h"

#include <windows.h>
#include <cstdio>
#include <fstream>
#include <intrin.h>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "config.h"
#include "editor/history.h"
#include "editor/level.h"
#include "editor/mission.h"
#include "editor/model.h"
#include "editor/npcs.h"
#include "editor/playtest.h"
#include "engine/npc.h"
#include "imgui.h"
#include "hook.h"
#include "fov.h"
#include "game.h"
#include "log.h"
#include "overlay/overlay.h"
#include "scene.h"
#include "screenshot.h"
#include "script/lua_vm.h"

namespace h2 {

namespace {

struct Held {
    int code;      // DIK code, or 0x100 + mouse button
    double until;  // NowMs
};
std::mutex g_mutex;
std::vector<Held> g_held;
long g_moveX, g_moveY, g_wheel;  // pending mouse motion and wheel
std::string g_shotPath;

// editor (ImGui) mouse events, one step per frame: move, press, release
struct UiStep {
    int kind;  // 0 move, 1 button down, 2 button up, 3 wheel, 4 key down, 5 key up, 6 char
    float x, y;
    int button;
};
std::vector<UiStep> g_ui;
double g_nextPoll;

std::string ModsPath(const char* name) { return GameDir() + "Mods\\" + name; }

void Prune(double now)
{
    for (size_t i = 0; i < g_held.size();)
        if (g_held[i].until <= now)
            g_held.erase(g_held.begin() + i);
        else
            ++i;
}

void RunCommandFile()
{
    std::string path = ModsPath("_cmd.lua");
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return;
    std::stringstream ss;
    ss << f.rdbuf();
    f.close();
    DeleteFileA(path.c_str());
    std::string code = ss.str();
    lua_State* L = script::State();
    std::string out;
    int base = lua_gettop(L);
    if (luaL_loadbuffer(L, code.c_str(), code.size(), "@_cmd.lua") != LUA_OK ||
        script::PCall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
        out = std::string("error: ") + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
    } else {
        for (int i = base + 1; i <= lua_gettop(L); ++i) {
            size_t n;
            const char* s = luaL_tolstring(L, i, &n);
            out += std::string(s, n) + "\n";
            lua_pop(L, 1);
        }
        if (out.empty())
            out = "ok\n";
    }
    lua_settop(L, base);
    Log("devcmd: %s", out.c_str());
    std::ofstream o(ModsPath("_cmd.out"), std::ios::binary | std::ios::trunc);
    o << out;
}

void Poll()
{
    double now = NowMs();
    if (now < g_nextPoll)
        return;
    g_nextPoll = now + 200.0;
    RunCommandFile();
}

// h2.dev_savegame(slot) / h2.dev_loadgame(slot): ZEngineDataBase::SaveGame
// 0x41AAD0 / LoadGame 0x41AF70 (Save\<slot>.1 .. .3; testing the saved state)
bool CallSlot(uintptr_t fn, unsigned slot)
{
    auto sys = reinterpret_cast<char*>(game::SysInterface());
    void* engine = sys ? *reinterpret_cast<void**>(sys + 0x98) : nullptr;
    if (!engine)
        return false;
    __try {
        return reinterpret_cast<bool(__thiscall*)(void*, unsigned)>(fn)(engine, slot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int SaveGameCmd(lua_State* L)
{
    lua_pushboolean(L, CallSlot(0x41AAD0, unsigned(luaL_checkinteger(L, 1))));
    return 1;
}

int LoadGameCmd(lua_State* L)
{
    lua_pushboolean(L, CallSlot(0x41AF70, unsigned(luaL_checkinteger(L, 1))));
    return 1;
}

// h2.dev_levelend(completed): the base level's own mission end (as its scripts do)
int LevelEnd(lua_State* L)
{
    lua_pushboolean(L, editor::BaseLevelEnd(lua_toboolean(L, 1) != 0));
    return 1;
}

// h2.dev_loadscene(name): asks the engine to switch scenes (as the menus do)
int LoadScene(lua_State* L)
{
    lua_pushboolean(L, RequestScene(luaL_checkstring(L, 1)));
    return 1;
}

// h2.dev_key(dik, seconds): holds a DirectInput key (DIK_ code)
int Key(lua_State* L)
{
    int dik = int(luaL_checkinteger(L, 1)) & 0xFF;
    double secs = luaL_optnumber(L, 2, 0.1);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_held.push_back({dik, NowMs() + secs * 1000.0});
    return 0;
}

// h2.dev_mouse(button, seconds): holds a mouse button (0 left, 1 right, 2 middle)
int Mouse(lua_State* L)
{
    int button = int(luaL_checkinteger(L, 1)) & 3;
    double secs = luaL_optnumber(L, 2, 0.1);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_held.push_back({0x100 + button, NowMs() + secs * 1000.0});
    return 0;
}

// h2.dev_look(dx, dy): mouse motion (mickeys) the game reads next
int Look(lua_State* L)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_moveX += long(luaL_checkinteger(L, 1));
    g_moveY += long(luaL_optinteger(L, 2, 0));
    return 0;
}

// h2.dev_wheel(notches): mouse wheel (+ up / - down), 120 per notch
int Wheel(lua_State* L)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_wheel += long(luaL_checkinteger(L, 1)) * 120;
    return 0;
}

// h2.dev_tracecams([frames]): logs every camera the renderer draws
int TraceCams(lua_State* L)
{
    TraceCameras(int(luaL_optinteger(L, 1, 1)));
    return 0;
}

// h2.dev_viewdistance(scale)
int ViewDist(lua_State* L)
{
    SetViewDistance(float(luaL_checknumber(L, 1)));
    return 0;
}

// h2.dev_click(x, y [, button [, double]]): moves the editor's mouse to screen
// pixel (x, y) and clicks (0 left, 1 right) - drives the editor UI and its
// clicks in the world
int Click(lua_State* L)
{
    float x = float(luaL_checknumber(L, 1)), y = float(luaL_checknumber(L, 2));
    int button = int(luaL_optinteger(L, 3, 0));
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ui.push_back({0, x, y, button});
    g_ui.push_back({0, x, y, button});
    g_ui.push_back({1, x, y, button});
    g_ui.push_back({2, x, y, button});
    if (lua_toboolean(L, 4)) {
        g_ui.push_back({1, x, y, button});
        g_ui.push_back({2, x, y, button});
    }
    return 0;
}

// h2.dev_hover(x, y): moves the editor's mouse
int Hover(lua_State* L)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ui.push_back({0, float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)), 0});
    return 0;
}

// h2.dev_type(text): types into the focused editor field
int Type(lua_State* L)
{
    std::string text = luaL_checkstring(L, 1);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (unsigned char c : text)
        g_ui.push_back({6, 0, 0, c});
    return 0;
}

// h2.dev_uikey(imguiKey): presses an ImGui key in the editor (e.g. 525 = Enter)
int UiKey(lua_State* L)
{
    int key = int(luaL_checkinteger(L, 1));
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ui.push_back({4, 0, 0, key});
    g_ui.push_back({5, 0, 0, key});
    return 0;
}

// h2.dev_shot(name): saves Screens\<name>.bmp at the next frame
int Shot(lua_State* L)
{
    std::string dir = GameDir() + "Screens";
    CreateDirectoryA(dir.c_str(), nullptr);
    g_shotPath = dir + "\\" + luaL_checkstring(L, 1) + ".bmp";
    return 0;
}

// h2.dev_panel(name): shows an editor panel (left tab / right section)
// h2.dev_level(op [, arg]): "new" name | "save" | "load" folder | "apply" | "revert" | "undo" | "redo" | "info"
int Level(lua_State* L)
{
    std::string op = luaL_checkstring(L, 1);
    std::string arg = luaL_optstring(L, 2, "");
    std::string err;
    editor::LevelDocument& d = editor::CurrentLevel();
    if (op == "new") {
        editor::NewLevel(arg.empty() ? "Test" : arg, CurrentScene());
    } else if (op == "save") {
        if (!editor::SaveLevel(&err))
            return luaL_error(L, "save: %s", err.c_str());
    } else if (op == "load") {
        if (!editor::LoadLevel(arg, &err))
            return luaL_error(L, "load: %s", err.c_str());
    } else if (op == "apply") {
        lua_pushinteger(L, editor::ApplyLevel());
        return 1;
    } else if (op == "revert") {
        editor::RevertLevel();
    } else if (op == "undo") {
        editor::Undo();
    } else if (op == "redo") {
        editor::Redo();
    } else if (op != "info") {
        return luaL_error(L, "unknown op %s", op.c_str());
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "%s folder=%s scene=%s objects=%d spawned=%d npcs=%d npc_edits=%d lists=%d%s",
             d.name.c_str(), d.folder.c_str(), d.baseScene.c_str(), int(d.objects.size()), int(d.spawned.size()),
             int(d.npcs.size()), int(d.npcEdits.size()), int(d.lists.size()), d.dirty ? " (unsaved)" : "");
    lua_pushstring(L, buf);
    return 1;
}

int Panel(lua_State* L)
{
    overlay::ShowPanel(luaL_checkstring(L, 1));
    return 0;
}

// h2.dev_place_npc(record, x, y, z [, behaviour]): places a character as the
// NPCs panel does (shown until the map loads again) -> its object name
int PlaceNpcCmd(lua_State* L)
{
    editor::NpcSpec spec;
    spec.record = int(luaL_checkinteger(L, 1));
    float m[9] = {0, 0, 1, 0, 1, 0, 1, 0, 0};
    memcpy(spec.transform.mat, m, sizeof(m));
    for (int i = 0; i < 3; ++i)
        spec.transform.pos[i] = float(luaL_checknumber(L, 2 + i));
    if (lua_isstring(L, 5)) {
        if (!editor::BehaviourTemplate(lua_tostring(L, 5), spec.behaviour))
            return luaL_error(L, "no behaviour %s", lua_tostring(L, 5));
        spec.hasBehaviour = true;
    }
    std::string err;
    engine::ZGEOM* g = editor::PlaceNpc(spec, &err);
    if (!g)
        return luaL_error(L, "place: %s", err.c_str());
    lua_pushstring(L, engine::SafeString(g->name).c_str());
    return 1;
}

// h2.dev_setscript(path, script, decl): starts the script on the character
// (decl as a script record has it: "LONG a=1;ZGEOMREF b;"; values: the
// defaults written there, 0 else) -> true / error text
int SetScriptCmd(lua_State* L)
{
    engine::ZGEOM* g = editor::Resolve(luaL_checkstring(L, 1));
    if (!g)
        return luaL_error(L, "no such object");
    engine::ScriptInfo s;
    s.name = luaL_checkstring(L, 2);
    std::string decl = luaL_checkstring(L, 3);
    for (size_t start = 0; start < decl.size();) {
        size_t semi = decl.find(';', start);
        if (semi == std::string::npos)
            semi = decl.size();
        std::string item = decl.substr(start, semi - start), value;
        start = semi + 1;
        if (size_t eq = item.find('='); eq != std::string::npos) {
            value = item.substr(eq + 1);
            item.resize(eq);
        }
        size_t sp = item.rfind(' ');
        if (sp == std::string::npos)
            continue;
        engine::ScriptParam p;
        p.typeName = item.substr(0, sp);
        p.name = item.substr(sp + 1);
        if (p.typeName == "LONG") {
            p.type = engine::ParamType::Long;
            p.bits = uint32_t(atoi(value.c_str()));
        } else if (p.typeName == "FLOAT") {
            p.type = engine::ParamType::Float;
            float f = float(atof(value.c_str()));
            memcpy(&p.bits, &f, 4);
        } else {
            p.type = engine::ParamType::Ref;
        }
        s.params.push_back(p);
    }
    std::vector<uint8_t> block = engine::BuildScriptBlock(s);
    std::string err;
    if (!engine::SetScript(g, &block, &err)) {
        lua_pushstring(L, err.c_str());
        return 1;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// h2.dev_test(): the toolbar's Test
int TestCmd(lua_State*)
{
    editor::TestLevel();
    return 0;
}

// h2.dev_editor(open): opens / closes the editor overlay
int Editor(lua_State* L)
{
    overlay::SetOpen(lua_toboolean(L, 1));
    return 0;
}

} // namespace

// ---- sound without focus (automated testing: the game keeps playing in the background) ----

struct DsBufferDesc {
    DWORD dwSize;
    DWORD dwFlags;
};
using CreateSoundBuffer_t = HRESULT(__stdcall*)(void* ds, DsBufferDesc* desc, void** buffer, void* outer);
CreateSoundBuffer_t oCreateSoundBuffer;
HRESULT __stdcall hkCreateSoundBuffer(void* ds, DsBufferDesc* desc, void** buffer, void* outer)
{
    constexpr DWORD kPrimary = 0x1, kGlobalFocus = 0x8000;
    if (desc && !(desc->dwFlags & kPrimary))
        desc->dwFlags |= kGlobalFocus;
    return oCreateSoundBuffer(ds, desc, buffer, outer);
}

void HookSoundObject(void* ds)
{
    if (!ds || oCreateSoundBuffer)
        return;
    void* fn = (*reinterpret_cast<void***>(ds))[3];  // IDirectSound::CreateSoundBuffer
    Hook("IDirectSound::CreateSoundBuffer", fn, hkCreateSoundBuffer, &oCreateSoundBuffer);
}

using DirectSoundCreate_t = HRESULT(WINAPI*)(const GUID*, void**, void*);
DirectSoundCreate_t oDirectSoundCreate, oDirectSoundCreate8;
HRESULT WINAPI hkDirectSoundCreate(const GUID* g, void** ds, void* outer)
{
    HRESULT hr = oDirectSoundCreate(g, ds, outer);
    if (SUCCEEDED(hr) && ds)
        HookSoundObject(*ds);
    return hr;
}
HRESULT WINAPI hkDirectSoundCreate8(const GUID* g, void** ds, void* outer)
{
    HRESULT hr = oDirectSoundCreate8(g, ds, outer);
    if (SUCCEEDED(hr) && ds)
        HookSoundObject(*ds);
    return hr;
}

void InstallBackgroundSound()
{
    if (!LoadLibraryA("dsound.dll"))
        return;
    HookApi("dsound.dll", "DirectSoundCreate", hkDirectSoundCreate, &oDirectSoundCreate);
    HookApi("dsound.dll", "DirectSoundCreate8", hkDirectSoundCreate8, &oDirectSoundCreate8);
}

// ---- callers of a function per object (reverse engineering) ----
// h2.dev_callers(addr, argc): records the return addresses of thiscall calls
// of the function (argc stack arguments, 0-3) per `this`, until
// h2.dev_callers_dump(obj [, clear]) reads them ("ret count" lines).
std::mutex g_callersMutex;
std::map<std::pair<uintptr_t, uintptr_t>, int> g_callers;  // (this, return address) -> calls
void* g_callersOrig[4];

template <int N>
uint32_t __fastcall CallersHook(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c)
{
    {
        std::lock_guard<std::mutex> lock(g_callersMutex);
        ++g_callers[{reinterpret_cast<uintptr_t>(self), reinterpret_cast<uintptr_t>(_ReturnAddress())}];
    }
    if (N == 0)
        return reinterpret_cast<uint32_t(__fastcall*)(void*, void*)>(g_callersOrig[N])(self, edx);
    if (N == 1)
        return reinterpret_cast<uint32_t(__fastcall*)(void*, void*, uint32_t)>(g_callersOrig[N])(self, edx, a);
    if (N == 2)
        return reinterpret_cast<uint32_t(__fastcall*)(void*, void*, uint32_t, uint32_t)>(g_callersOrig[N])(self, edx,
                                                                                                         a, b);
    return reinterpret_cast<uint32_t(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t)>(g_callersOrig[N])(
        self, edx, a, b, c);
}
// fastcall stub per argc so the callee cleans the right number of bytes
uint32_t __fastcall Callers0(void* s, void* e) { return CallersHook<0>(s, e, 0, 0, 0); }
uint32_t __fastcall Callers1(void* s, void* e, uint32_t a) { return CallersHook<1>(s, e, a, 0, 0); }
uint32_t __fastcall Callers2(void* s, void* e, uint32_t a, uint32_t b) { return CallersHook<2>(s, e, a, b, 0); }
uint32_t __fastcall Callers3(void* s, void* e, uint32_t a, uint32_t b, uint32_t c) { return CallersHook<3>(s, e, a, b, c); }

int Callers(lua_State* L)
{
    uintptr_t addr = uintptr_t(luaL_checkinteger(L, 1));
    int argc = int(luaL_optinteger(L, 2, 0));
    void* detour = argc == 0 ? (void*)&Callers0 : argc == 1 ? (void*)&Callers1 : argc == 2 ? (void*)&Callers2 : (void*)&Callers3;
    MH_STATUS st = MH_CreateHook(reinterpret_cast<void*>(addr), detour, &g_callersOrig[argc > 3 ? 3 : argc]);
    if (st == MH_OK)
        st = MH_EnableHook(reinterpret_cast<void*>(addr));
    lua_pushstring(L, MH_StatusToString(st));
    return 1;
}

int CallersDump(lua_State* L)
{
    uintptr_t obj = uintptr_t(luaL_checkinteger(L, 1));
    bool clear = lua_toboolean(L, 2);
    std::string out;
    std::lock_guard<std::mutex> lock(g_callersMutex);
    for (auto it = g_callers.begin(); it != g_callers.end();) {
        if (it->first.first == obj) {
            char line[48];
            snprintf(line, sizeof(line), "%08X %d\n", unsigned(it->first.second), it->second);
            out += line;
        }
        it = clear ? g_callers.erase(it) : std::next(it);
    }
    lua_pushstring(L, out.c_str());
    return 1;
}

// ---- what collision queries return (reverse engineering) ----
// h2.dev_coll(): hooks the collision manager's node gathering (RenderD3D:
// vfunc 0x38 rooms + dynamic, vfunc 0x7c dynamic tree); h2.dev_coll_seen(node)
// -> how often that node was returned since.
std::map<uintptr_t, int> g_collSeen;
using Gather10 = int(__fastcall*)(void*, void*, void**, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                  uint32_t, uint32_t, uint32_t);
using Gather8 = int(__fastcall*)(void*, void*, void**, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                 uint32_t);
Gather10 oGatherNodes;
Gather8 oGatherDynamic;
void Seen(void** out, int n)
{
    std::lock_guard<std::mutex> lock(g_callersMutex);
    for (int i = 0; i < n && i < 4096; ++i)
        ++g_collSeen[reinterpret_cast<uintptr_t>(out[i])];
}
int __fastcall hkGatherNodes(void* self, void* edx, void** out, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                             uint32_t e, uint32_t f, uint32_t g, uint32_t h, uint32_t i)
{
    int n = oGatherNodes(self, edx, out, a, b, c, d, e, f, g, h, i);
    Seen(out, n);
    {
        std::lock_guard<std::mutex> lock(g_callersMutex);
        ++g_callers[{1, reinterpret_cast<uintptr_t>(_ReturnAddress())}];
    }
    return n;
}
int __fastcall hkGatherDynamic(void* self, void* edx, void** out, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                               uint32_t e, uint32_t f, uint32_t g)
{
    int n = oGatherDynamic(self, edx, out, a, b, c, d, e, f, g);
    Seen(out, n);
    {
        std::lock_guard<std::mutex> lock(g_callersMutex);
        ++g_callers[{2, reinterpret_cast<uintptr_t>(_ReturnAddress())}];
    }
    return n;
}
int Coll(lua_State* L)
{
    uintptr_t render = reinterpret_cast<uintptr_t>(GetModuleHandleA("RenderD3D.dll"));
    if (!render)
        return luaL_error(L, "no RenderD3D.dll");
    bool ok = Hook("GatherNodes", render + 0x28C80, &hkGatherNodes, &oGatherNodes) &&
              Hook("GatherDynamic", render + 0x28DE0, &hkGatherDynamic, &oGatherDynamic);
    lua_pushboolean(L, ok);
    return 1;
}
int CollSeen(lua_State* L)
{
    uintptr_t node = uintptr_t(luaL_checkinteger(L, 1));
    std::lock_guard<std::mutex> lock(g_callersMutex);
    auto it = g_collSeen.find(node);
    lua_pushinteger(L, it == g_collSeen.end() ? 0 : it->second);
    return 1;
}

void InstallDevCommands()
{
    if (!g_config.devCommands)
        return;
    InstallBackgroundSound();
    script::Register("dev_loadscene", LoadScene);
    script::Register("dev_savegame", SaveGameCmd);
    script::Register("dev_loadgame", LoadGameCmd);
    script::Register("dev_levelend", LevelEnd);
    script::Register("dev_key", Key);
    script::Register("dev_mouse", Mouse);
    script::Register("dev_look", Look);
    script::Register("dev_wheel", Wheel);
    script::Register("dev_tracecams", TraceCams);
    script::Register("dev_viewdistance", ViewDist);
    script::Register("dev_shot", Shot);
    script::Register("dev_editor", Editor);
    script::Register("dev_place_npc", PlaceNpcCmd);
    script::Register("dev_test", TestCmd);
    script::Register("dev_setscript", SetScriptCmd);
    script::Register("dev_panel", Panel);
    script::Register("dev_level", Level);
    script::Register("dev_click", Click);
    script::Register("dev_hover", Hover);
    script::Register("dev_type", Type);
    script::Register("dev_uikey", UiKey);
    script::Register("dev_callers", Callers);
    script::Register("dev_callers_dump", CallersDump);
    script::Register("dev_coll", Coll);
    script::Register("dev_coll_seen", CollSeen);
    Log("dev commands on: Mods\\_cmd.lua");
}

bool DevInjecting()
{
    if (!g_config.devCommands)
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    Prune(NowMs());
    return !g_held.empty() || g_moveX || g_moveY || g_wheel;
}

void DevInjectKeyboard(unsigned char* keys)
{
    if (!g_config.devCommands)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    Prune(NowMs());
    for (const Held& h : g_held)
        if (h.code < 0x100)
            keys[h.code] = 0x80;
}

void DevInjectMouse(long* dx, long* dy, unsigned char* buttons, long* wheel)
{
    if (!g_config.devCommands)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    Prune(NowMs());
    for (const Held& h : g_held)
        if (h.code >= 0x100)
            buttons[h.code - 0x100] = 0x80;
    *dx += g_moveX;
    *dy += g_moveY;
    g_moveX = g_moveY = 0;
    if (wheel) {
        *wheel += g_wheel;
        g_wheel = 0;
    }
}

// the injected mouse position stays a while (the backend adds the OS cursor every frame)
float g_holdX, g_holdY;
double g_holdUntil;

void DevUiStep()
{
    if (!g_config.devCommands || !ImGui::GetCurrentContext())
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    ImGuiIO& io = ImGui::GetIO();
    if (NowMs() < g_holdUntil)
        io.AddMousePosEvent(g_holdX, g_holdY);
    if (g_ui.empty())
        return;
    UiStep s = g_ui.front();
    g_ui.erase(g_ui.begin());
    if (s.kind <= 2) {
        g_holdX = s.x;
        g_holdY = s.y;
        g_holdUntil = NowMs() + 2000.0;
    }
    switch (s.kind) {
    case 0: io.AddMousePosEvent(s.x, s.y); break;
    // the backend adds the OS cursor every frame while the game is in front: clicks bring their position
    case 1: io.AddMousePosEvent(s.x, s.y); io.AddMouseButtonEvent(s.button, true); break;
    case 2: io.AddMousePosEvent(s.x, s.y); io.AddMouseButtonEvent(s.button, false); break;
    case 4: io.AddKeyEvent(ImGuiKey(s.button), true); break;
    case 5: io.AddKeyEvent(ImGuiKey(s.button), false); break;
    case 6: io.AddInputCharacter(unsigned(s.button)); break;
    }
}

void DevOnPresent(void* device)
{
    TraceFrameDone();
    if (g_config.devCommands)
        Poll();  // at Present, like the editor UI (outside the game logic)
    if (g_shotPath.empty())
        return;
    std::string path = g_shotPath;
    g_shotPath.clear();
    if (!SaveBackBuffer(device, path) && !SaveScreenshot(device, path))
        Log("devcmd: screenshot %s failed", path.c_str());
}

} // namespace h2
