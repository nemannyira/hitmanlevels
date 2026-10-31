#pragma once

namespace h2::editor {

// Simple mode's guide (docs/simple-mode.md): the left panel's steps, Mission > People > Items >
// The map > Try it > Save & share, each with its controls and Back / Next.
void InstallGuide();
// Simple mode's toolbar: Undo, Redo, Save, Try it, the Advanced mode switch.
void DrawSimpleToolbar();
// The guide shows this step (0 Mission, 1 People, ... 5 Save & share).
void GuideGoToStep(int step);

} // namespace h2::editor
