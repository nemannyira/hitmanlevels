#include "memapi.h"

#include <cstring>

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "lua_vm.h"

namespace h2::script {

namespace {

template <typename T>
bool SafeRead(uintptr_t addr, T& out)
{
    __try {
        out = *reinterpret_cast<const T*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t Addr(lua_State* L, int i) { return static_cast<uintptr_t>(luaL_checkinteger(L, i)); }

template <typename T>
int ReadNum(lua_State* L)
{
    T v;
    if (!SafeRead(Addr(L, 1), v)) {
        lua_pushnil(L);
        return 1;
    }
    if constexpr (std::is_floating_point_v<T>)
        lua_pushnumber(L, v);
    else
        lua_pushinteger(L, static_cast<lua_Integer>(v));
    return 1;
}

int Str(lua_State* L)
{
    uintptr_t a = Addr(L, 1);
    int max = static_cast<int>(luaL_optinteger(L, 2, 256));
    std::string s;
    for (int i = 0; i < max; ++i) {
        char c;
        if (!SafeRead(a + i, c)) {
            if (i == 0) {
                lua_pushnil(L);
                return 1;
            }
            break;
        }
        if (!c)
            break;
        s += c;
    }
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}

int Hex(lua_State* L)
{
    uintptr_t a = Addr(L, 1);
    int len = static_cast<int>(luaL_checkinteger(L, 2));
    std::string out;
    char buf[8];
    for (int i = 0; i < len; ++i) {
        unsigned char c;
        if (!SafeRead(a + i, c)) {
            out += "??";
        } else {
            snprintf(buf, sizeof(buf), "%02X", c);
            out += buf;
        }
        out += (i % 16 == 15) ? "\n" : " ";
    }
    lua_pushstring(L, out.c_str());
    return 1;
}

// __thiscall is callee-cleaned, so the exact argument count must be passed.
template <typename... A>
uint32_t Invoke(uintptr_t fn, void* self, A... a)
{
    return reinterpret_cast<uint32_t(__thiscall*)(void*, A...)>(fn)(self, a...);
}

bool SafeCall(uintptr_t fn, void* self, const uint32_t* a, int n, uint32_t& r)
{
    __try {
        switch (n) {
        case 0: r = Invoke(fn, self); break;
        case 1: r = Invoke(fn, self, a[0]); break;
        case 2: r = Invoke(fn, self, a[0], a[1]); break;
        case 3: r = Invoke(fn, self, a[0], a[1], a[2]); break;
        case 4: r = Invoke(fn, self, a[0], a[1], a[2], a[3]); break;
        case 5: r = Invoke(fn, self, a[0], a[1], a[2], a[3], a[4]); break;
        default: r = Invoke(fn, self, a[0], a[1], a[2], a[3], a[4], a[5]); break;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int CallImpl(lua_State* L, uintptr_t fn, void* self, int firstArg)
{
    uint32_t a[6] = {};
    int n = 0;
    for (int i = firstArg; i <= lua_gettop(L) && n < 6; ++i)
        a[n++] = static_cast<uint32_t>(luaL_checkinteger(L, i));
    uint32_t r;
    if (!SafeCall(fn, self, a, n, r)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, r);
    return 1;
}

int VCall(lua_State* L)
{
    uintptr_t obj = Addr(L, 1);
    uintptr_t off = Addr(L, 2);
    uintptr_t vtbl, fn;
    if (!SafeRead(obj, vtbl) || !SafeRead(vtbl + off, fn)) {
        lua_pushnil(L);
        return 1;
    }
    return CallImpl(L, fn, reinterpret_cast<void*>(obj), 3);
}

int Call(lua_State* L) { return CallImpl(L, Addr(L, 1), reinterpret_cast<void*>(Addr(L, 2)), 3); }

bool SafeWrite(uintptr_t addr, const void* v, size_t n)
{
    __try {
        memcpy(reinterpret_cast<void*>(addr), v, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// h2.mem.wf32(addr, value) / h2.mem.w32(addr, value): writes (reverse engineering only)
int WriteF32(lua_State* L)
{
    float v = float(luaL_checknumber(L, 2));
    lua_pushboolean(L, SafeWrite(Addr(L, 1), &v, 4));
    return 1;
}

int Write32(lua_State* L)
{
    uint32_t v = uint32_t(luaL_checkinteger(L, 2));
    lua_pushboolean(L, SafeWrite(Addr(L, 1), &v, 4));
    return 1;
}

} // namespace

void RegisterMemApi()
{
    lua_State* L = State();
    const luaL_Reg fns[] = {
        {"u8", ReadNum<uint8_t>},   {"u16", ReadNum<uint16_t>}, {"u32", ReadNum<uint32_t>},
        {"i32", ReadNum<int32_t>},  {"f32", ReadNum<float>},    {"str", Str},
        {"hex", Hex},               {"vcall", VCall},           {"call", Call},
        {"wf32", WriteF32},         {"w32", Write32},
        {nullptr, nullptr},
    };
    lua_getglobal(L, "h2");
    luaL_newlib(L, fns);
    lua_setfield(L, -2, "mem");
    lua_pop(L, 1);
}

} // namespace h2::script
