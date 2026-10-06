#include "lua_vm.h"

#include <windows.h>
#include <cstdlib>
#include <fstream>
#include <sstream>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "../log.h"

namespace h2::script {

namespace {

lua_State* L;
std::vector<ConsoleLine> g_console;

constexpr size_t kMemoryLimit = 256u << 20;
constexpr ULONGLONG kCallBudgetMs = 3000;
size_t g_memory;          // bytes the state has
int g_depth;              // calls into Lua under way (PCall)
ULONGLONG g_callStart;    // when the outermost began

void* LimitedAlloc(void*, void* ptr, size_t osize, size_t nsize)
{
    if (!ptr)
        osize = 0;  // a type tag, not a size
    if (nsize == 0) {
        free(ptr);
        g_memory -= osize;
        return nullptr;
    }
    if (nsize > osize && g_memory + (nsize - osize) > kMemoryLimit)
        return nullptr;  // Lua raises "not enough memory"
    void* p = realloc(ptr, nsize);
    if (p)
        g_memory = g_memory - osize + nsize;
    return p;
}

int Panic(lua_State* s)
{
    Log("lua: unprotected error: %s", lua_tostring(s, -1) ? lua_tostring(s, -1) : "?");
    return 0;  // abort
}

void CountHook(lua_State* s, lua_Debug*)
{
    if (g_depth > 0 && GetTickCount64() - g_callStart > kCallBudgetMs)
        luaL_error(s, "script stopped: it ran longer than %d s", int(kCallBudgetMs / 1000));
}

// load(chunk [, name [, mode [, env]]]) of the base library, source text only
int LoadText(lua_State* s)
{
    int n = lua_gettop(s);
    lua_pushvalue(s, lua_upvalueindex(1));
    lua_pushvalue(s, 1);
    if (n >= 2)
        lua_pushvalue(s, 2);
    else
        lua_pushnil(s);
    lua_pushliteral(s, "t");
    if (n >= 4)
        lua_pushvalue(s, 4);
    lua_call(s, n >= 4 ? 4 : 3, LUA_MULTRET);
    return lua_gettop(s) - n;
}
constexpr size_t kMaxConsoleLines = 2000;

int Print(lua_State* s)
{
    std::string line;
    int n = lua_gettop(s);
    for (int i = 1; i <= n; ++i) {
        size_t len;
        const char* str = luaL_tolstring(s, i, &len);
        if (i > 1)
            line += "\t";
        line.append(str, len);
        lua_pop(s, 1);
    }
    ConsolePrint(LineKind::Output, line);
    return 0;
}

int Traceback(lua_State* s)
{
    const char* msg = lua_tostring(s, 1);
    luaL_traceback(s, s, msg ? msg : "(error object is not a string)", 1);
    return 1;
}

// pcall with traceback; prints results (if any) when printResults is set
bool Run(int nargs, bool printResults)
{
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, Traceback);
    lua_insert(L, base);
    int status = PCall(L, nargs, printResults ? LUA_MULTRET : 0, base);
    lua_remove(L, base);
    if (status != LUA_OK) {
        ConsolePrint(LineKind::Error, lua_tostring(L, -1) ? lua_tostring(L, -1) : "error");
        lua_pop(L, 1);
        return false;
    }
    if (printResults) {
        int n = lua_gettop(L) - base + 1;
        if (n > 0) {
            lua_getglobal(L, "print");
            lua_insert(L, base);
            PCall(L, n, 0, 0);
        }
    }
    return true;
}

void OpenSandboxedLibs()
{
    const luaL_Reg libs[] = {
        {LUA_GNAME, luaopen_base},         {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},   {LUA_UTF8LIBNAME, luaopen_utf8}, {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_OSLIBNAME, luaopen_os},
    };
    for (const auto& lib : libs) {
        luaL_requiref(L, lib.name, lib.func, 1);
        lua_pop(L, 1);
    }
    // base: no loading code from disk behind our back
    for (const char* name : {"dofile", "loadfile"}) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
    // load: source text only (a precompiled chunk can corrupt the VM)
    lua_getglobal(L, "load");
    lua_pushcclosure(L, LoadText, 1);
    lua_setglobal(L, "load");
    // string.dump is of no use then
    lua_getglobal(L, "string");
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);
    // os: keep only harmless functions
    lua_getglobal(L, "os");
    for (const char* name : {"execute", "exit", "getenv", "remove", "rename", "setlocale", "tmpname"}) {
        lua_pushnil(L);
        lua_setfield(L, -2, name);
    }
    lua_pop(L, 1);
}

} // namespace

void Init()
{
    if (L)
        return;
    L = lua_newstate(LimitedAlloc, nullptr);
    lua_atpanic(L, Panic);
    lua_sethook(L, CountHook, LUA_MASKCOUNT, 100000);
    OpenSandboxedLibs();
    lua_pushcfunction(L, Print);
    lua_setglobal(L, "print");
    lua_newtable(L);
    lua_setglobal(L, "h2");
    ConsolePrint(LineKind::Output, LUA_RELEASE " ready");
}

lua_State* State() { return L; }

int PCall(lua_State* s, int nargs, int nresults, int errfunc)
{
    if (g_depth++ == 0)
        g_callStart = GetTickCount64();
    int status = lua_pcall(s, nargs, nresults, errfunc);
    --g_depth;
    return status;
}

void ExecConsole(const std::string& code)
{
    ConsolePrint(LineKind::Input, "> " + code);
    // expression first ("1+1" prints 2), then as a statement
    std::string expr = "return " + code;
    if (luaL_loadbuffer(L, expr.c_str(), expr.size(), "=console") == LUA_OK) {
        Run(0, true);
        return;
    }
    lua_pop(L, 1);
    if (luaL_loadbuffer(L, code.c_str(), code.size(), "=console") != LUA_OK) {
        ConsolePrint(LineKind::Error, lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    Run(0, true);
}

bool ExecChunk(const std::string& code, const std::string& chunkName)
{
    std::string name = "@" + chunkName;
    if (luaL_loadbuffer(L, code.c_str(), code.size(), name.c_str()) != LUA_OK) {
        ConsolePrint(LineKind::Error, lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return Run(0, false);
}

bool ExecFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::stringstream ss;
    ss << f.rdbuf();
    return ExecChunk(ss.str(), path);
}

const std::vector<ConsoleLine>& Console() { return g_console; }

void ConsolePrint(LineKind kind, const std::string& text)
{
    Log("lua%s %s", kind == LineKind::Error ? " error:" : ":", text.c_str());
    // split multi-line output so the console can colour and wrap each line
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line))
        g_console.push_back({kind, line});
    if (g_console.size() > kMaxConsoleLines)
        g_console.erase(g_console.begin(), g_console.begin() + (g_console.size() - kMaxConsoleLines));
}

void ClearConsole() { g_console.clear(); }

void Register(const char* name, CFunction fn)
{
    lua_getglobal(L, "h2");
    lua_pushcfunction(L, fn);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

} // namespace h2::script
