#include "devcmd.h"

#include <windows.h>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "config.h"
#include "fov.h"
#include "game.h"
#include "log.h"
#include "overlay/overlay.h"
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
        lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
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

// h2.dev_loadscene(name): asks the engine to switch scenes (as the menus do)
int LoadScene(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    auto sys = reinterpret_cast<char*>(game::SysInterface());
    void* engine = sys ? *reinterpret_cast<void**>(sys + 0x98) : nullptr;
    if (!engine)
        return luaL_error(L, "no engine");
    // a level change: every scene on the stack goes, then the new one (as the menus do)
    using PopAll_t = void(__thiscall*)(void*);
    reinterpret_cast<PopAll_t>(game::ZEngineDataBase_PopAllScenes)(engine);
    using PushScene_t = void*(__thiscall*)(void*, const char*);
    void* r = reinterpret_cast<PushScene_t>(game::ZEngineDataBase_PushScene)(engine, name);
    lua_pushboolean(L, r != nullptr);
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

// h2.dev_shot(name): saves Screens\<name>.bmp at the next frame
int Shot(lua_State* L)
{
    std::string dir = GameDir() + "Screens";
    CreateDirectoryA(dir.c_str(), nullptr);
    g_shotPath = dir + "\\" + luaL_checkstring(L, 1) + ".bmp";
    return 0;
}

// h2.dev_editor(open): opens / closes the editor overlay
int Editor(lua_State* L)
{
    overlay::SetOpen(lua_toboolean(L, 1));
    return 0;
}

} // namespace

void InstallDevCommands()
{
    if (!g_config.devCommands)
        return;
    script::Register("dev_loadscene", LoadScene);
    script::Register("dev_key", Key);
    script::Register("dev_mouse", Mouse);
    script::Register("dev_look", Look);
    script::Register("dev_wheel", Wheel);
    script::Register("dev_tracecams", TraceCams);
    script::Register("dev_shot", Shot);
    script::Register("dev_editor", Editor);
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

void DevOnPresent(void* device)
{
    TraceFrameDone();
    if (g_config.devCommands)
        Poll();  // at Present, like the editor UI (outside the game logic)
    if (g_shotPath.empty())
        return;
    std::string path = g_shotPath;
    g_shotPath.clear();
    if (!SaveScreenshot(device, path))
        Log("devcmd: screenshot %s failed", path.c_str());
}

} // namespace h2
