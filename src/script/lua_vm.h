#pragma once
#include <string>
#include <vector>

struct lua_State;

namespace h2::script {

enum class LineKind { Input, Output, Error };

struct ConsoleLine {
    LineKind kind;
    std::string text;
};

// The single Lua 5.4 state shared by the console and level scripts.
// Sandboxed: base, table, string, math, utf8, coroutine and a reduced os
// (clock/time/date); no io, no loading native modules, `load` takes source
// text only (no precompiled chunks: they can break out of the VM), at most
// kMemoryLimit bytes, and calls stop after kCallBudgetMs (PCall).
void Init();
lua_State* State();

// Run a console line REPL-style: expressions print their value.
void ExecConsole(const std::string& code);
// Run a chunk (file contents); errors go to the console.
bool ExecChunk(const std::string& code, const std::string& chunkName);
bool ExecFile(const std::string& path);

const std::vector<ConsoleLine>& Console();
void ConsolePrint(LineKind kind, const std::string& text);
void ClearConsole();

// Every call from the game into Lua (events, chunks, the console): a script
// running longer than the budget is stopped with an error (a level's script
// must not hang the game). As lua_pcall.
int PCall(lua_State* s, int nargs, int nresults, int errfunc);

// Register C functions into the global "h2" table.
using CFunction = int (*)(lua_State*);
void Register(const char* name, CFunction fn);

} // namespace h2::script
