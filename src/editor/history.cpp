#include "history.h"

#include <vector>

#include "../log.h"
#include "../scene.h"
#include "level.h"

namespace h2::editor {

namespace {

struct Entry {
    std::string path;
    ObjectState before, after;
    std::string what;
};

std::vector<Entry> g_undo, g_redo;
constexpr size_t kMaxEntries = 500;

engine::ZGEOM* g_continuousGeom;
ObjectState g_continuousBefore;
bool g_hooked;

void Hook()
{
    if (g_hooked)
        return;
    g_hooked = true;
    OnScenePushed([](const std::string&) { ClearHistory(); });  // paths belong to the scene
}

bool ApplyEntry(const Entry& e, bool forward)
{
    engine::ZGEOM* geom = Resolve(e.path);
    if (!geom || !IsLive(geom)) {
        Log("history: %s not found", e.path.c_str());
        return false;
    }
    const ObjectState& from = forward ? e.before : e.after;
    const ObjectState& to = forward ? e.after : e.before;
    SetState(geom, to);
    RecordEdit(geom, from, to);
    return true;
}

void Push(Entry e)
{
    g_undo.push_back(std::move(e));
    if (g_undo.size() > kMaxEntries)
        g_undo.erase(g_undo.begin());
    g_redo.clear();
}

} // namespace

void Change(engine::ZGEOM* geom, const ObjectState& before, const ObjectState& after, const char* what)
{
    Hook();
    if (!IsLive(geom) || before == after)
        return;
    std::string path = PathOf(geom);
    SetState(geom, after);
    RecordEdit(geom, before, after);
    if (!path.empty())
        Push({path, before, after, what});
}

void BeginContinuous(engine::ZGEOM* geom)
{
    Hook();
    if (g_continuousGeom || !IsLive(geom))
        return;
    g_continuousGeom = geom;
    g_continuousBefore = GetState(geom);
}

void EndContinuous(engine::ZGEOM* geom, const char* what)
{
    if (!g_continuousGeom || geom != g_continuousGeom)
        return;
    g_continuousGeom = nullptr;
    if (!IsLive(geom))
        return;
    ObjectState after = GetState(geom);
    if (after == g_continuousBefore)
        return;
    RecordEdit(geom, g_continuousBefore, after);
    std::string path = PathOf(geom);
    if (!path.empty())
        Push({path, g_continuousBefore, after, what});
}

bool InContinuous() { return g_continuousGeom != nullptr; }

bool Undo()
{
    if (g_undo.empty())
        return false;
    Entry e = g_undo.back();
    g_undo.pop_back();
    ApplyEntry(e, false);
    g_redo.push_back(std::move(e));
    return true;
}

bool Redo()
{
    if (g_redo.empty())
        return false;
    Entry e = g_redo.back();
    g_redo.pop_back();
    ApplyEntry(e, true);
    g_undo.push_back(std::move(e));
    return true;
}

bool CanUndo() { return !g_undo.empty(); }
bool CanRedo() { return !g_redo.empty(); }
std::string UndoLabel() { return g_undo.empty() ? "" : g_undo.back().what; }
std::string RedoLabel() { return g_redo.empty() ? "" : g_redo.back().what; }

void ClearHistory()
{
    g_undo.clear();
    g_redo.clear();
    g_continuousGeom = nullptr;
}

} // namespace h2::editor
