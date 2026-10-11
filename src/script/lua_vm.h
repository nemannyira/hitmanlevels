#pragma once
#include <deque>
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
// The last errors printed (at most 20, their first line), oldest first;
// `serial` counts every error ever printed (ErrorSerial: the last one's).
struct ScriptError {
    unsigned serial;
    std::string text;
};
const std::deque<ScriptError>& RecentErrors();
unsigned ErrorSerial();
// Lines printed to the console ever (the console keeps the last 2000).
size_t ConsoleSerial();
// Names for the console's Tab: the fields of the table at `path` ("" the
// globals, "h2", "string") or, `method`, an object's methods, that start
// with `partial`; a table's name ends in ".", a function's in "(". Read
// raw: no script code runs.
std::vector<std::string> Complete(const std::string& path, const std::string& partial, bool method);
void ClearConsole();

// Every call from the game into Lua (events, chunks, the console): a script
// running longer than the budget is stopped with an error (a level's script
// must not hang the game). As lua_pcall.
int PCall(lua_State* s, int nargs, int nresults, int errfunc);
// Every PCall has a time budget of its own (3 s; a call made inside another
// gets its own, the outer one's goes on when it returns). A call out of time
// is stopped for good: every instruction fails until it returns.
bool LastCallTimedOut();          // the last PCall that returned was stopped
bool CallTimeLeft();              // the call under way has time left
int TimeoutCount();               // calls stopped since the last reset
void ResetTimeouts();

// Register C functions into the global "h2" table.
using CFunction = int (*)(lua_State*);
void Register(const char* name, CFunction fn);

} // namespace h2::script
