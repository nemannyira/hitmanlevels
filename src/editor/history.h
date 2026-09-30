#pragma once
// Undo/redo of editor changes. Every change is an object state transition
// (before -> after) identified by the object's path, so it survives snapshot
// refreshes; each change is also recorded into the current level document.
#include <string>

#include "model.h"

namespace h2::editor {

// Applies `after` to the object and pushes the change.
void Change(engine::ZGEOM* geom, const ObjectState& before, const ObjectState& after, const char* what);

// Continuous edits (gizmo drags, slider drags): the object is modified live
// and a single history entry is created when the edit ends.
void BeginContinuous(engine::ZGEOM* geom);
void EndContinuous(engine::ZGEOM* geom, const char* what);
bool InContinuous();

bool Undo();
bool Redo();
bool CanUndo();
bool CanRedo();
std::string UndoLabel();
std::string RedoLabel();
void ClearHistory();

} // namespace h2::editor
