#include "guide.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "imgui.h"

#include "../engine/geoms.h"
#include "../engine/messages.h"
#include "../log.h"
#include "../screenshot.h"
#include "../overlay/overlay.h"
#include "../scene.h"
#include "freecam.h"
#include "history.h"
#include "launcher.h"
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
std::string g_saveStatus;  // the last save's result (step 6)
char g_readme[4096];         // the level's README.md (step 6), read when the level's folder changes
std::string g_readmeFolder;
std::string g_pictureStatus;

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
    char buf[128];
    if (int scripts = UserScriptCount()) {
        snprintf(buf, sizeof(buf), "%d script%s", scripts, scripts == 1 ? "" : "s");
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
    if (IsEscalation()) {
        snprintf(buf, sizeof(buf), "stages (an escalation: stage %d of %d is shown, what you add comes in it)",
                 CurrentStage(), StageCount());
        has.push_back(buf);
    }
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

// an object's name to read from its path (a character's part: with its group's)
std::string NameOfPath(const std::string& path)
{
    size_t slash = path.rfind('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (slash != std::string::npos && slash > 0 && (name == "Ground" || name.rfind('#', 0) == 0)) {
        size_t up = path.rfind('/', slash - 1);
        size_t from = up == std::string::npos ? 0 : up + 1;
        name = path.substr(from, slash - from);
    }
    return name;
}

// one row: the name (a click selects it and flies there), what changed, a button at the right
bool ChangeRow(const std::string& name, const std::string& what, const char* button, engine::ZGEOM* geom)
{
    float bw = ImGui::CalcTextSize(button).x + ImGui::GetStyle().FramePadding.x * 2;
    std::string label = name + "  (" + what + ")";
    if (ImGui::Selectable(label.c_str(), geom && Selected() == geom, ImGuiSelectableFlags_AllowOverlap,
                          ImVec2(ImGui::GetContentRegionAvail().x - bw - ImGui::GetStyle().ItemSpacing.x, 0)) &&
        geom && IsLive(geom)) {
        Select(geom);
        FlyCameraTo(GetTransform(geom).pos);
    }
    ImGui::SameLine(ImGui::GetContentRegionMax().x - bw);
    return ImGui::SmallButton(button);
}

// what the level changed of the map, each with its undo (#35)
void DrawMapChanges()
{
    const LevelDocument& L = CurrentLevel();
    ImGui::SeparatorText("What you changed");
    std::string revert, door, removeCopy;
    int rows = 0;
    for (const auto& [path, e] : L.objects) {
        engine::ZGEOM* g = Resolve(path);
        if (g && ActorOf(g))
            continue;  // (people: step 2)
        std::string what;
        if (e.hasOriginal && e.state.hidden != e.original.hidden)
            what = e.state.hidden ? "hidden" : "shown";
        if (!e.hasOriginal || memcmp(&e.state.transform, &e.original.transform, sizeof(Transform)) != 0)
            what += what.empty() ? "moved" : ", moved";
        ImGui::PushID(path.c_str());
        if (ChangeRow(NameOfPath(path), what, "Undo this", g))
            revert = path;
        ImGui::PopID();
        ++rows;
    }
    for (const auto& [path, locked] : L.doors) {
        ImGui::PushID(("door" + path).c_str());
        if (ChangeRow(NameOfPath(path), locked ? "locked" : "unlocked", "Undo this", Resolve(path)))
            door = path;
        ImGui::PopID();
        ++rows;
    }
    for (const SpawnedObject& s : L.spawned) {
        if (s.item)
            continue;  // (items: step 3)
        engine::ZGEOM* g = LevelObject(s.id);
        ImGui::PushID(s.id.c_str());
        std::string name = s.point ? "a place (" + s.id.substr(0, s.id.find('+')) + ")" : s.id.substr(0, s.id.find('+'));
        if (ChangeRow(name, "added", "Remove", g) && g)
            removeCopy = s.id;
        ImGui::PopID();
        ++rows;
    }
    if (!rows)
        ImGui::TextDisabled("Nothing yet.");
    if (!revert.empty())
        RevertObjectEdit(revert);
    if (!door.empty())
        ForgetLevelDoor(door);
    if (!removeCopy.empty())
        if (engine::ZGEOM* g = LevelObject(removeCopy)) {
            Select(g);
            DeleteSelection();
        }
}

void DrawMapStep()
{
    engine::ZGEOM* sel = Selected();
    ImGui::SeparatorText("The selected object");
    engine::ZGEOM* hitman = engine::Player();
    if (!sel || !IsLive(sel)) {
        ImGui::TextWrapped("Click an object in the world (a door, a crate, a car...).");
    } else if (hitman && (sel == hitman || ActorOf(sel) == hitman)) {
        ImGui::TextWrapped("Hitman: his clothes are step 1's, the people are step 2's. Click an object of the map.");
    } else if (ActorOf(sel)) {
        ImGui::TextWrapped("A person: step 2 (People) adds and takes them out. Click an object of the map.");
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
        if (ImGui::Button("Onto the surface (G)"))
            BeginGrab(sel);
        if (engine::ZGEOM* door = engine::DoorOf(sel)) {
            bool locked = engine::DoorLocked(door) == 1;
            if (ImGui::Button(locked ? "Unlock the door" : "Lock the door"))
                SetLevelDoorLock(door, !locked);
            ImGui::SameLine();
            ImGui::TextDisabled(locked ? "(locked: only its key opens it)" : "(a door)");
        }
    }
    DrawMapChanges();
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

std::string LevelFile(const char* name)
{
    return GameDir() + "Mods\\" + CurrentLevel().folder + "\\" + name;
}

// the picture of the view: the frame without the editor, 640 wide (package.h: a preview.png)
void SavePicture(void* device)
{
    std::string path = LevelFile("preview.png");
    g_pictureStatus = SavePreviewPng(device, path, 640) ? "preview.png: taken" : "preview.png: not written";
    Log("guide: %s (%s)", g_pictureStatus.c_str(), path.c_str());
}

// the description and the picture a package carries (README.md, preview.png in the level's folder)
void DrawDescription()
{
    const LevelDocument& L = CurrentLevel();
    if (g_readmeFolder != L.folder) {
        g_readmeFolder = L.folder;
        g_readme[0] = 0;
        g_pictureStatus.clear();
        std::ifstream f(LevelFile("README.md"), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        snprintf(g_readme, sizeof(g_readme), "%s", ss.str().c_str());
    }
    ImGui::SeparatorText("Description (README.md)");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("What the mission is about and how to play it: players see it with the package.");
    ImGui::PopTextWrapPos();
    ImGui::InputTextMultiline("##readme", g_readme, sizeof(g_readme), ImVec2(-1, ImGui::GetTextLineHeight() * 6));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        std::ofstream f(LevelFile("README.md"), std::ios::binary | std::ios::trunc);
        f << g_readme;
    }
    if (ImGui::IsItemHovered() && !g_readme[0])
        ImGui::SetTooltip("What the mission is about, how to play it: the players see it with the package");
    ImGui::SeparatorText("Picture (preview.png)");
    bool have = GetFileAttributesA(LevelFile("preview.png").c_str()) != INVALID_FILE_ATTRIBUTES;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", g_pictureStatus.empty() ? (have ? "preview.png: there" : "none yet")
                                                      : g_pictureStatus.c_str());
    ImGui::TextDisabled("Fly the camera to a good view first (hold the right mouse button, W A S D).");
    ImGui::PopTextWrapPos();
    if (ImGui::Button(have ? "Take it again from this view" : "Take a picture of this view", ImVec2(-1, 0)))
        overlay::RequestCleanFrame(SavePicture);
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
            g_saveStatus = SaveLevelAs(g_saveName, &err) ? "saved" : "not saved: " + err;
        }
        ImGui::EndDisabled();
    } else {
        ImGui::Text("%s%s", L.name.c_str(), L.dirty ? " (changes not saved)" : " (saved)");
        ImGui::BeginDisabled(!L.dirty);
        if (ImGui::Button("Save (Ctrl+S)", ImVec2(-1, 0))) {
            std::string err;
            g_saveStatus = SaveLevel(&err) ? "saved" : "not saved: " + err;
        }
        ImGui::EndDisabled();
    }
    if (!g_saveStatus.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", g_saveStatus.c_str());
        ImGui::PopTextWrapPos();
    }
    if (!L.folder.empty()) {
        DrawDescription();
        DrawShareSection();
    }
}

void DrawGuide()
{
    const LevelDocument& L = CurrentLevel();
    bool gameMap = !CurrentScene().empty() && MapTitle(CurrentScene()) != CurrentScene();  // (not a menu, an intro)
    if (L.folder.empty() && L.baseScene.empty()) {  // no level open
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted("No level is open.");
        ImGui::TextDisabled("Main menu > Level Editor: a new level on one of the game's maps, or one saved before.");
        ImGui::PopTextWrapPos();
        if (gameMap) {
            ImGui::Spacing();
            ImGui::TextWrapped("Or make one on this map (%s):", MapTitle(CurrentScene()).c_str());
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##newname", "its name", g_saveName, sizeof(g_saveName));
            ImGui::BeginDisabled(!g_saveName[0]);
            if (ImGui::Button("Make a level of this map", ImVec2(-1, 0))) {
                NewLevel(g_saveName, CurrentScene());
                std::string err;
                g_saveStatus = SaveLevel(&err) ? "" : "not saved: " + err;
                g_step = 0;
            }
            ImGui::EndDisabled();
        }
        return;
    }
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
    } else {
        ImGui::NewLine();  // (the last step: no Next)
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled(FreeCamEnabled() ? "Camera: hold the right mouse button and use W A S D (the wheel: speed)"
                                         : "Camera: the game's; Free camera (toolbar) flies anywhere");
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
    // beside the object, on the side of the screen with room (right of it on the left half, else left of it)
    sx = std::clamp(sx, 40.0f, io.DisplaySize.x - 40.0f);  // (off the screen: at its edge)
    bool left = sx > io.DisplaySize.x * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(left ? sx - 40.0f : sx + 40.0f, std::clamp(sy - 40.0f, 60.0f, io.DisplaySize.y - 160.0f)),
                            ImGuiCond_Always, ImVec2(left ? 1.0f : 0.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.88f);
    if (ImGui::Begin("##selcard", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav)) {
        engine::ZGEOM* hitman = engine::Player();
        if (hitman && (sel == hitman || ActorOf(sel) == hitman)) {  // (not an object to take, hide or copy)
            ImGui::TextUnformatted("Hitman");
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 18);
            ImGui::TextDisabled("He starts where the map starts him (Try it: Play from the start); his clothes: "
                                "step 1, Hitman starts.");
            ImGui::PopTextWrapPos();
            ImGui::End();
            return;
        }
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
            bool out = HiddenByLevel(actor);  // (one the map starts hidden is not offered: it is the map's)
            if (actor != engine::Player() && (out || !IsHidden(actor)) &&
                ImGui::SmallButton(out ? "Put back" : "Take out of the map")) {
                ObjectState before = GetState(actor), after = before;
                after.hidden = !before.hidden;
                Change(actor, before, after, after.hidden ? "hide" : "show");
            }
        } else {
            if (engine::ZGEOM* door = engine::DoorOf(sel)) {
                bool locked = engine::DoorLocked(door) == 1;
                if (ImGui::SmallButton(locked ? "Unlock the door" : "Lock the door"))
                    SetLevelDoorLock(door, !locked);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("A locked door opens only for the holder of its key");
                ImGui::SameLine();
            }
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
    if (CurrentLevel().folder.empty() && CurrentLevel().baseScene.empty()) {  // no level open (the guide says so)
        float w = ImGui::CalcTextSize("Advanced mode").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SameLine(ImGui::GetWindowWidth() - w - ImGui::GetStyle().WindowPadding.x);
        if (ImGui::Button("Advanced mode"))
            overlay::SetSimpleMode(false);
        return;
    }
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
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    bool free = FreeCamEnabled();
    if (ImGui::Checkbox("Free camera", &free))
        SetFreeCamEnabled(free);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("On: fly anywhere (hold the right mouse button, W A S D, Q / E down / up, the wheel:\n"
                          "speed). Off: the game's camera behind Hitman.");
    ImGui::SameLine();
    if (ImGui::Button("To Hitman"))
        if (engine::ZGEOM* hitman = engine::Player())
            FlyCameraTo(GetTransform(hitman).pos);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The free camera flies to Hitman (F: to the selected object)");
    ImGui::SameLine();
    float w = ImGui::CalcTextSize("Advanced mode").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SameLine(ImGui::GetWindowWidth() - w - ImGui::GetStyle().WindowPadding.x);
    if (ImGui::Button("Advanced mode"))
        overlay::SetSimpleMode(false);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Every tool of the editor: the scene's objects, characters' settings, scripts,\n"
                          "vehicles, the sky, stages... (View > Simple mode comes back)");
}

void GuideGoToStep(int step)
{
    g_step = step;
}

void InstallGuide()
{
    overlay::AddPanel({"Guide", &g_visible, DrawGuide, overlay::Dock::Left, overlay::kSimple});
    static bool always = true;
    overlay::AddPanel({nullptr, &always, DrawSelectionCard});
}

} // namespace h2::editor
