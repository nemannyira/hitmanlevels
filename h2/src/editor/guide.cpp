#include "guide.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"

#include "../engine/geoms.h"
#include "../engine/messages.h"
#include "../overlay/overlay.h"
#include "../scene.h"
#include "history.h"
#include "level.h"
#include "mission.h"
#include "model.h"
#include "models_panel.h"
#include "npc_panel.h"
#include "npcs.h"
#include "placement.h"
#include "playtest.h"
#include "scene_panel.h"
#include "stages.h"
#include "viewport.h"

namespace h2::editor {

namespace {

bool g_visible = true;
int g_step = 0;
char g_saveName[64];

enum class State { Done, Missing, Optional };

struct Step {
    const char* title;
    const char* hint;  // one line: what to do here
};
const Step kSteps[] = {
    {"Mission", "What does the player have to do? Keep the original mission or set objectives of your own."},
    {"People", "Add guards, civilians or targets, or take some of the map's people out."},
    {"Items", "Weapons, keys and other things for Hitman to find: choose one, then click where it goes."},
    {"The map", "Change what is there: click an object to move, turn, hide or copy it, or add new ones."},
    {"Try it", "Play the level as a player will. Problems the editor sees are listed with their fixes."},
    {"Save & share", "Save the level, then make a package to give to other players."},
};
constexpr int kStepCount = int(sizeof(kSteps) / sizeof(kSteps[0]));

bool OnLevelMap()
{
    const LevelDocument& L = CurrentLevel();
    return !CurrentScene().empty() && (L.baseScene.empty() || L.baseScene == CurrentScene());
}

State StepState(int step)
{
    const LevelDocument& L = CurrentLevel();
    switch (step) {
    case 0:
        return MissionStepDone() ? State::Done : State::Missing;
    case 1: {
        bool hidden = false;
        for (const auto& [path, e] : L.objects)
            hidden |= e.state.hidden;
        return !L.npcs.empty() || hidden ? State::Done : State::Optional;
    }
    case 2:
        for (const SpawnedObject& s : L.spawned)
            if (s.item)
                return State::Done;
        return State::Optional;
    case 3:
        return L.objects.empty() && L.spawned.empty() ? State::Optional : State::Done;
    case 4:
        return LevelProblems().empty() ? State::Done : State::Missing;
    default:
        return !L.folder.empty() && !L.dirty ? State::Done : State::Missing;
    }
}

void DrawStepBar()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    float avail = ImGui::GetContentRegionAvail().x, x = 0.0f;
    for (int i = 0; i < kStepCount; ++i) {
        char label[64];
        snprintf(label, sizeof(label), "%d %s", i + 1, kSteps[i].title);
        float w = ImGui::CalcTextSize(label).x + style.FramePadding.x * 2;
        if (i && x + w + style.ItemSpacing.x <= avail)
            ImGui::SameLine();
        else
            x = 0.0f;
        x += w + style.ItemSpacing.x;
        State s = StepState(i);
        ImVec4 color = s == State::Done      ? ImVec4(0.25f, 0.55f, 0.3f, 1.0f)
                       : s == State::Missing ? ImVec4(0.7f, 0.5f, 0.15f, 1.0f)
                                             : ImVec4(0.3f, 0.3f, 0.33f, 1.0f);
        if (i == g_step)
            color = ImVec4(color.x + 0.15f, color.y + 0.15f, color.z + 0.15f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x + 0.1f, color.y + 0.1f, color.z + 0.1f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, i == g_step ? 2.0f : 0.0f);
        if (ImGui::SmallButton(label))
            g_step = i;
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n%s", kSteps[i].hint,
                              s == State::Done      ? "(done)"
                              : s == State::Missing ? "(something is missing)"
                                                    : "(optional)");
    }
}

// what the level has that the guide does not show: Advanced mode edits it
void DrawAdvancedNote()
{
    const LevelDocument& L = CurrentLevel();
    std::vector<std::string> has;
    char buf[64];
    if (!L.scripts.empty()) {
        snprintf(buf, sizeof(buf), "%d script%s", int(L.scripts.size()), L.scripts.size() == 1 ? "" : "s");
        has.push_back(buf);
    }
    if (!L.vehicles.empty())
        has.push_back("driving cars");
    if (!L.newLists.empty())
        has.push_back("routes of their own");
    if (L.blankMap)
        has.push_back("no buildings");
    else if (L.emptyMap)
        has.push_back("an emptied map");
    if (!L.skyLevel.empty())
        has.push_back("another sky");
    if (IsEscalation())
        has.push_back("stages (an escalation)");
    if (!L.music.empty())
        has.push_back("music");
    if (!L.ownMapOf.empty())
        has.push_back("a map of its own");
    if (has.empty())
        return;
    std::string text = "This level also has ";
    for (size_t i = 0; i < has.size(); ++i)
        text += (i ? (i + 1 == has.size() ? " and " : ", ") : "") + has[i];
    text += ": they stay as they are; Advanced mode edits them.";
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", text.c_str());
    ImGui::PopTextWrapPos();
}

void DrawMapStep()
{
    engine::ZGEOM* sel = Selected();
    ImGui::SeparatorText("The selected object");
    if (!sel || !IsLive(sel)) {
        ImGui::TextWrapped("Click an object in the world (a door, a crate, a car...).");
    } else {
        ImGui::TextUnformatted(engine::SafeString(sel->name).c_str());
        bool turning = GizmoTurning();
        if (ImGui::RadioButton("Move (W)", !turning))
            SetGizmoTurning(false);
        ImGui::SameLine();
        if (ImGui::RadioButton("Turn (E)", turning))
            SetGizmoTurning(true);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Drag the arrows (the rings when turning) on the object");
        bool hidden = IsHidden(sel);
        if (ImGui::Button(hidden ? "Show it again" : "Hide it (H)")) {
            ObjectState before = GetState(sel), after = before;
            after.hidden = !hidden;
            Change(sel, before, after, after.hidden ? "hide" : "show");
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy (Ctrl+D)"))
            DuplicateSelection();
        if (FindSpawned(sel)) {
            ImGui::SameLine();
            if (ImGui::Button("Remove (Del)"))
                DeleteSelection();
        }
        ImGui::SameLine();
        if (ImGui::Button("Onto the surface (G)"))
            BeginGrab(sel);
    }
    const LevelDocument& L = CurrentLevel();
    ImGui::TextDisabled("%d objects of the map changed, %d added", int(L.objects.size()), int(L.spawned.size()));
    ImGui::SeparatorText("Add an object");
    if (ImGui::CollapsingHeader("The map's models (click one, then click in the world)"))
        DrawModelsList();
}

void DrawTryStep()
{
    ImGui::SeparatorText("Play it");
    int pending = PendingNpcCount();
    if (ImGui::Button("Play from the start", ImVec2(-1, 0)))
        TestLevelFrom(TestStart::LevelStart);
    if (ImGui::Button("Play from where the camera looks", ImVec2(-1, 0)))
        TestLevelFrom(TestStart::Camera);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("The level is saved first. %s opens the editor again.%s", overlay::EditorKeyName().c_str(),
                        pending ? " The new people come to life then." : "");
    ImGui::PopTextWrapPos();
    DrawProblemsList();
}

void DrawShareStep()
{
    LevelDocument& L = CurrentLevel();
    ImGui::SeparatorText("Save");
    if (L.folder.empty()) {
        ImGui::TextWrapped("Give the level a name:");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##name", g_saveName, sizeof(g_saveName));
        ImGui::BeginDisabled(!g_saveName[0]);
        if (ImGui::Button("Save", ImVec2(-1, 0))) {
            std::string err;
            SaveLevelAs(g_saveName, &err);
        }
        ImGui::EndDisabled();
    } else {
        ImGui::Text("%s%s", L.name.c_str(), L.dirty ? " (changes not saved)" : " (saved)");
        ImGui::BeginDisabled(!L.dirty);
        if (ImGui::Button("Save (Ctrl+S)", ImVec2(-1, 0))) {
            std::string err;
            SaveLevel(&err);
        }
        ImGui::EndDisabled();
    }
    if (!L.folder.empty())
        DrawShareSection();
}

void DrawGuide()
{
    if (CurrentScene().empty()) {
        ImGui::TextWrapped("Load a map first: main menu > Level Editor > New level.");
        return;
    }
    DrawStepBar();
    ImGui::Separator();
    if (!OnLevelMap()) {
        ImGui::TextWrapped("The level is for %s; another map is loaded.", CurrentLevel().baseScene.c_str());
        return;
    }
    if (g_step < 0 || g_step >= kStepCount)
        g_step = 0;
    ImGui::Text("%d. %s", g_step + 1, kSteps[g_step].title);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", kSteps[g_step].hint);
    ImGui::PopTextWrapPos();
    if (g_step == 0)
        DrawAdvancedNote();

    // the step's controls in a region of their own: Back / Next stay at the bottom
    float footer = ImGui::GetFrameHeightWithSpacing() * 2 + ImGui::GetTextLineHeightWithSpacing() * 2;
    if (ImGui::BeginChild("step", ImVec2(0, -footer))) {
        switch (g_step) {
        case 0: DrawSimpleMission(); break;
        case 1: DrawSimplePeople(); break;
        case 2: DrawSimpleItems(); break;
        case 3: DrawMapStep(); break;
        case 4: DrawTryStep(); break;
        default: DrawShareStep(); break;
        }
    }
    ImGui::EndChild();
    ImGui::Separator();
    float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    ImGui::BeginDisabled(g_step == 0);
    if (ImGui::Button("< Back", ImVec2(half, 0)))
        --g_step;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (g_step + 1 < kStepCount) {
        char next[64];
        snprintf(next, sizeof(next), "Next: %s >", kSteps[g_step + 1].title);
        if (ImGui::Button(next, ImVec2(half, 0)))
            ++g_step;
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Camera: hold the right mouse button and use W A S D (the wheel: speed)");
    ImGui::PopTextWrapPos();
}


// the selection's card: next to the selected object on screen, what can be done with it (#35)
void DrawSelectionCard()
{
    engine::ZGEOM* sel = Selected();
    if (!overlay::SimpleMode() || !sel || !IsLive(sel) || Placing() || NpcPlacing() || MissionPicking() ||
        !OnLevelMap() || ImGui::IsMouseDown(ImGuiMouseButton_Right))
        return;
    Transform t = GetTransform(sel);
    float sx, sy;
    if (!WorldToScreen(t.pos, sx, sy))
        return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(std::min(sx + 40.0f, io.DisplaySize.x - 260.0f), std::max(sy - 40.0f, 60.0f)),
                            ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.88f);
    if (ImGui::Begin("##selcard", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav)) {
        engine::ZGEOM* actor = ActorOf(sel);
        bool placed = actor && IsPlacedNpc(actor);
        ImGui::TextUnformatted(actor ? PersonName(actor).c_str() : engine::SafeString(sel->name).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled(actor ? (placed ? "(a new person)" : "(a person of the map)")
                            : FindSpawned(sel) ? "(added)" : "(of the map)");
        DrawMissionForSelection(sel);  // make it a target, Hitman has to take it / get here, the exit
        ImGui::Separator();
        if (actor && placed) {
            if (ImGui::SmallButton("Remove")) {
                std::string err;
                RemoveNpc(actor, &err);
            }
        } else if (actor) {
            if (actor != engine::Player() && ImGui::SmallButton(IsHidden(actor) ? "Put back" : "Take out of the map")) {
                ObjectState before = GetState(actor), after = before;
                after.hidden = !before.hidden;
                Change(actor, before, after, after.hidden ? "hide" : "show");
            }
        } else {
            bool turning = GizmoTurning();
            if (ImGui::SmallButton(turning ? "Move (W)" : "Turn (E)"))
                SetGizmoTurning(!turning);
            ImGui::SameLine();
            if (ImGui::SmallButton(IsHidden(sel) ? "Show" : "Hide (H)")) {
                ObjectState before = GetState(sel), after = before;
                after.hidden = !before.hidden;
                Change(sel, before, after, after.hidden ? "hide" : "show");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy"))
                DuplicateSelection();
            if (FindSpawned(sel)) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove"))
                    DeleteSelection();
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("More..."))
            overlay::SetSimpleMode(false);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Advanced mode: the Inspector and every setting of it");
    }
    ImGui::End();
}
} // namespace

void DrawSimpleToolbar()
{
    ImGui::BeginDisabled(!CanUndo());
    if (ImGui::Button("Undo"))
        Undo();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(CanUndo() ? "Undo %s (Ctrl+Z)" : "Nothing to undo", UndoLabel().c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(!CanRedo());
    if (ImGui::Button("Redo"))
        Redo();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(CanRedo() ? "Redo %s (Ctrl+Y)" : "Nothing to redo", RedoLabel().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    DrawSaveControl();
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::Button(PendingNpcCount() ? "Try it *" : "Try it"))
        TestLevelFrom(TestStart::LevelStart);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Saves the level and plays it from its start (step 5 has more)");
    ImGui::SameLine();
    float w = ImGui::CalcTextSize("Advanced mode").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SameLine(ImGui::GetWindowWidth() - w - ImGui::GetStyle().WindowPadding.x);
    if (ImGui::Button("Advanced mode"))
        overlay::SetSimpleMode(false);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Every tool of the editor: the scene's objects, characters' settings, scripts,\n"
                          "vehicles, the sky, stages... (View > Simple mode comes back)");
}

void InstallGuide()
{
    overlay::AddPanel({"Guide", &g_visible, DrawGuide, overlay::Dock::Left, overlay::kSimple});
    static bool always = true;
    overlay::AddPanel({nullptr, &always, DrawSelectionCard});
}

} // namespace h2::editor
