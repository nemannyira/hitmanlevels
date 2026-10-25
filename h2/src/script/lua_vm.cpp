#include "lua_vm.h"

#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
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
constexpr size_t kUntrustedMemoryLimit = 64u << 20;  // a level from elsewhere (SetUntrusted)
constexpr ULONGLONG kUntrustedBudgetMs = 1000;
constexpr const char* kHiddenTools = "h2fix.hidden_tools";  // registry: h2's fields taken out while untrusted
bool g_untrusted;
std::string g_untrustedTag;
size_t MemoryLimit() { return g_untrusted ? kUntrustedMemoryLimit : kMemoryLimit; }
ULONGLONG CallBudgetMs() { return g_untrusted ? kUntrustedBudgetMs : kCallBudgetMs; }
constexpr int kHookCount = 100000;  // instructions between time checks
size_t g_memory;          // bytes the state has
int g_depth;              // calls into Lua under way (PCall)
ULONGLONG g_callStart;    // when the outermost began
bool g_stopping;          // its time is up: every instruction fails until it returns
bool g_lastTimedOut;
int g_timeouts;

void* LimitedAlloc(void*, void* ptr, size_t osize, size_t nsize)
{
    if (!ptr)
        osize = 0;  // a type tag, not a size
    if (nsize == 0) {
        free(ptr);
        g_memory -= osize;
        return nullptr;
    }
    if (nsize > osize && g_memory + (nsize - osize) > MemoryLimit())
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
    if (g_depth > 0 && (g_stopping || GetTickCount64() - g_callStart > CallBudgetMs())) {
        // A script could catch the error (pcall in a loop) and go on: from now
        // on every instruction of every thread fails until the call returns.
        if (!g_stopping) {
            g_stopping = true;
            lua_sethook(L, CountHook, LUA_MASKCOUNT, 1);  // new coroutines take the main thread's
        }
        lua_sethook(s, CountHook, LUA_MASKCOUNT, 1);
        luaL_error(s, "script stopped: it ran longer than %d s", int(CallBudgetMs() / 1000));
    }
}

// setmetatable without __gc: a finalizer runs whenever the collector does,
// also outside the calls the time limit watches (an endless one would hang
// the game)
int SafeSetMetatable(lua_State* s)
{
    if (lua_istable(s, 2)) {
        lua_pushliteral(s, "__gc");
        int t = lua_rawget(s, 2);
        lua_pop(s, 1);
        if (t != LUA_TNIL)
            return luaL_error(s, "setmetatable: __gc (finalizers) are not allowed");
    }
    lua_pushvalue(s, lua_upvalueindex(1));
    lua_insert(s, 1);
    lua_call(s, lua_gettop(s) - 1, 1);
    return 1;
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
    lua_getglobal(L, "setmetatable");
    lua_pushcclosure(L, SafeSetMetatable, 1);
    lua_setglobal(L, "setmetatable");
    // xpcall: the handler runs after the call, as plain code. Run as Lua's
    // message handler it is called again for the error the time limit
    // raises inside it, with hooks off: an endless handler would never stop.
    const char* xpcall = R"lua(
local pcall, pack, unpack = pcall, table.pack, table.unpack
return function(f, h, ...)
  if type(h) ~= 'function' then error('bad argument #2 to xpcall (function expected)', 2) end
  local r = pack(pcall(f, ...))
  if r[1] then return unpack(r, 1, r.n) end
  return false, h(r[2])
end
)lua";
    if (luaL_loadbuffer(L, xpcall, strlen(xpcall), "=xpcall") == LUA_OK && lua_pcall(L, 0, 1, 0) == LUA_OK)
        lua_setglobal(L, "xpcall");
    else
        lua_pop(L, 1);
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
    lua_sethook(L, CountHook, LUA_MASKCOUNT, kHookCount);
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
    ULONGLONG outerStart = g_callStart;
    bool outerStopping = g_stopping;
    g_callStart = GetTickCount64();
    g_stopping = false;
    ++g_depth;
    int status = lua_pcall(s, nargs, nresults, errfunc);
    --g_depth;
    g_lastTimedOut = g_stopping;
    if (g_stopping) {  // stopped: back to the usual checks
        ++g_timeouts;
        lua_sethook(L, CountHook, LUA_MASKCOUNT, kHookCount);
        if (s != L)
            lua_sethook(s, CountHook, LUA_MASKCOUNT, kHookCount);
    }
    // the outer call's time does not count the inner one's (one stopped
    // callback must not stop the code that called it)
    g_callStart = outerStart + (GetTickCount64() - g_callStart);
    g_stopping = outerStopping;
    if (g_stopping)
        lua_sethook(L, CountHook, LUA_MASKCOUNT, 1);
    return status;
}

bool LastCallTimedOut() { return g_lastTimedOut; }
bool CallTimeLeft() { return g_depth == 0 || (!g_stopping && GetTickCount64() - g_callStart <= CallBudgetMs()); }
int TimeoutCount() { return g_timeouts; }
void ResetTimeouts() { g_timeouts = 0; }

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

std::deque<ScriptError> g_errors;
unsigned g_errorSerial;
size_t g_consoleSerial;

size_t ConsoleSerial() { return g_consoleSerial; }

const std::deque<ScriptError>& RecentErrors() { return g_errors; }
unsigned ErrorSerial() { return g_errorSerial; }

void ConsolePrint(LineKind kind, const std::string& text)
{
    Log("lua%s %s", kind == LineKind::Error ? " error:" : ":", text.c_str());
    if (kind == LineKind::Error) {
        g_errors.push_back({++g_errorSerial, text.substr(0, std::min(text.find('\n'), size_t(1000)))});
        if (g_errors.size() > 20)
            g_errors.pop_front();
    }
    // split multi-line output so the console can colour and wrap each line
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        g_console.push_back({kind, line});
        ++g_consoleSerial;
    }
    if (g_console.size() > kMaxConsoleLines)
        g_console.erase(g_console.begin(), g_console.begin() + (g_console.size() - kMaxConsoleLines));
}

void ClearConsole() { g_console.clear(); }

namespace {

struct Completion {
    const std::string* path;
    const std::string* partial;
    bool method;
    std::vector<std::string> out;
};

int CompleteIn(lua_State* s)
{
    Completion* c = static_cast<Completion*>(lua_touserdata(s, 1));
    if (c->method) {
        luaL_getmetatable(s, "h2.object");
        if (!lua_istable(s, -1))
            return 0;
        lua_pushliteral(s, "__index");
        lua_rawget(s, -2);
    } else {
        lua_pushglobaltable(s);
        const std::string& path = *c->path;
        for (size_t start = 0; start < path.size();) {
            size_t dot = std::min(path.find('.', start), path.size());
            if (!lua_istable(s, -1))
                return 0;
            lua_pushlstring(s, path.data() + start, dot - start);
            lua_rawget(s, -2);
            lua_remove(s, -2);
            start = dot + 1;
        }
    }
    if (!lua_istable(s, -1))
        return 0;
    const std::string& partial = *c->partial;
    lua_pushnil(s);
    while (lua_next(s, -2)) {
        size_t n = 0;
        const char* k = lua_type(s, -2) == LUA_TSTRING ? lua_tolstring(s, -2, &n) : nullptr;
        if (k && c->out.size() < 1000 && n >= partial.size() && memcmp(k, partial.data(), partial.size()) == 0 &&
            (k[0] != '_' || (!partial.empty() && partial[0] == '_'))) {  // _path and the like: only asked for
            int t = lua_type(s, -1);
            c->out.push_back(std::string(k, n) + (t == LUA_TTABLE ? "." : t == LUA_TFUNCTION ? "(" : ""));
        }
        lua_pop(s, 1);
    }
    return 0;
}

bool IsName(const std::string& s, bool dots)
{
    for (char ch : s)
        if (!(isalnum(static_cast<unsigned char>(ch)) || ch == '_' || (dots && ch == '.')))
            return false;
    return true;
}

} // namespace

std::vector<std::string> Complete(const std::string& path, const std::string& partial, bool method)
{
    if (!L || path.size() > 200 || partial.size() > 100 || !IsName(path, true) || !IsName(partial, false))
        return {};
    Completion c{&path, &partial, method, {}};
    int top = lua_gettop(L);
    lua_pushcfunction(L, CompleteIn);
    lua_pushlightuserdata(L, &c);
    if (PCall(L, 1, 0, 0) != LUA_OK)
        c.out.clear();
    lua_settop(L, top);
    std::sort(c.out.begin(), c.out.end());
    return c.out;
}

void Register(const char* name, CFunction fn)
{
    lua_getglobal(L, "h2");
    lua_pushcfunction(L, fn);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

void SetUntrusted(bool on, const std::string& tag, bool keepDevTools)
{
    g_untrustedTag = on ? tag : "";
    if (on == g_untrusted || !L)
        return;
    g_untrusted = on;
    int top = lua_gettop(L);
    lua_getglobal(L, "h2");
    if (on) {  // the user's tools out of h2, into the registry
        lua_newtable(L);
        std::vector<std::string> names;
        lua_pushnil(L);
        while (lua_next(L, -3)) {
            lua_pop(L, 1);
            if (lua_type(L, -1) != LUA_TSTRING)
                continue;
            std::string name = lua_tostring(L, -1);
            if (name == "mem" || (!keepDevTools && name.rfind("dev_", 0) == 0))
                names.push_back(name);
        }
        for (const std::string& name : names) {
            lua_getfield(L, -2, name.c_str());
            lua_setfield(L, -2, name.c_str());
            lua_pushnil(L);
            lua_setfield(L, -3, name.c_str());
        }
        lua_setfield(L, LUA_REGISTRYINDEX, kHiddenTools);
        Log("lua: a downloaded level runs: %d MB, %d s a call, %zu of h2's tools taken out", int(MemoryLimit() >> 20),
            int(CallBudgetMs() / 1000), names.size());
    } else if (lua_getfield(L, LUA_REGISTRYINDEX, kHiddenTools) == LUA_TTABLE) {  // back
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            lua_pushvalue(L, -2);
            lua_insert(L, -2);
            lua_settable(L, -5);
        }
        lua_pushnil(L);
        lua_setfield(L, LUA_REGISTRYINDEX, kHiddenTools);
        Log("lua: the level is the user's own again: h2's tools back");
    }
    lua_settop(L, top);
}

bool Untrusted() { return g_untrusted; }
const std::string& UntrustedTag() { return g_untrustedTag; }

} // namespace h2::script
