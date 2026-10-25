#include "scene_panel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>

#include "imgui.h"

#include "../display.h"
#include "../engine/geoms.h"
#include "../engine/imports.h"
#include "../engine/inject.h"
#include "../engine/spawn.h"
#include "../fov.h"
#include "../game.h"
#include "../overlay/overlay.h"
#include "../scene.h"
#include "freecam.h"
#include "history.h"
#include "ingame_map.h"
#include "level.h"
#include "mission.h"
#include "model.h"
#include "placement.h"
#include "vehicles.h"
#include "viewport.h"

namespace h2::editor {

namespace {

bool g_sceneVisible = true;
bool g_inspectorVisible = true;
bool g_autoRefresh = true;
char g_filter[128];
engine::ZGEOM* g_selected;
bool g_scrollToSelected;
std::vector<int> g_matches;
std::string g_matchFilter;
const engine::SceneSnapshot* g_matchSnap;

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

const engine::SceneSnapshot& Snap() { return SceneModel(g_autoRefresh ? 2000.0 : 1e18); }

const engine::GeomInfo* FindInfo(engine::ZGEOM* geom)
{
    const auto& s = Snap();
    int i = s.Find(geom);
    return i >= 0 ? &s.geoms[i] : nullptr;
}

bool IsAncestorOfSelected(const engine::SceneSnapshot& s, int i)
{
    int sel = g_selected ? s.Find(g_selected) : -1;
    for (int k = 0; sel >= 0 && k < 64; ++k) {
        int parent = -1;
        for (int c = 0; c < int(s.geoms.size()) && parent < 0; ++c)
            if (s.geoms[sel].parent == s.geoms[c].node)
                parent = c;
        if (parent == i)
            return true;
        sel = parent;
    }
    return false;
}

void DrawTreeNode(const engine::SceneSnapshot& s, int i)
{
    const engine::GeomInfo& g = s.geoms[i];
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (g.children.empty())
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (g.geom == g_selected)
        flags |= ImGuiTreeNodeFlags_Selected;
    if (g_scrollToSelected && !g.children.empty() && IsAncestorOfSelected(s, i))
        ImGui::SetNextItemOpen(true);
    bool hidden = IsLive(g.geom) && IsHidden(g.geom);
    if (hidden)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    bool open = ImGui::TreeNodeEx(static_cast<void*>(g.node), flags, "%s  (%s)",
                                  g.name.empty() ? "<unnamed>" : g.name.c_str(), g.className.c_str());
    if (hidden)
        ImGui::PopStyleColor();
    if (g_scrollToSelected && g.geom == g_selected) {
        ImGui::SetScrollHereY();
        g_scrollToSelected = false;
    }
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        g_selected = g.geom;
    if (open && !g.children.empty()) {
        for (int c : g.children)
            DrawTreeNode(s, c);
        ImGui::TreePop();
    }
}

void DrawScenePanel()
{
    const engine::SceneSnapshot& s = Snap();
    if (ImGui::Button("Refresh"))
        SceneModel(0);
    ImGui::SameLine();
    ImGui::Checkbox("auto", &g_autoRefresh);
    ImGui::SameLine();
    ImGui::TextDisabled("%d objects", int(s.geoms.size()));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "filter by name or class", g_filter, sizeof(g_filter));

    if (ImGui::BeginChild("tree", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        std::string filter = Lower(g_filter);
        if (filter.empty()) {
            for (int r : s.roots)
                DrawTreeNode(s, r);
        } else {
            if (filter != g_matchFilter || g_matchSnap != &s || g_matches.size() > s.geoms.size()) {
                g_matchFilter = filter;
                g_matchSnap = &s;
                g_matches.clear();
                for (int i = 0; i < int(s.geoms.size()); ++i)
                    if (Lower(s.geoms[i].name).find(filter) != std::string::npos ||
                        Lower(s.geoms[i].className).find(filter) != std::string::npos)
                        g_matches.push_back(i);
            }
            ImGuiListClipper clipper;
            clipper.Begin(int(g_matches.size()));
            while (clipper.Step()) {
                for (int k = clipper.DisplayStart; k < clipper.DisplayEnd; ++k) {
                    if (g_matches[k] >= int(s.geoms.size()))
                        continue;
                    const auto& g = s.geoms[g_matches[k]];
                    char label[256];
                    snprintf(label, sizeof(label), "%s  (%s)##%p", g.name.empty() ? "<unnamed>" : g.name.c_str(),
                             g.className.c_str(), static_cast<void*>(g.node));
                    if (ImGui::Selectable(label, g.geom == g_selected))
                        g_selected = g.geom;
                }
            }
        }
    }
    ImGui::EndChild();
}

// Continuous edit of a widget: history entry when the widget is released.
void TrackContinuous(engine::ZGEOM* geom, const char* what)
{
    if (ImGui::IsItemActivated())
        BeginContinuous(geom);
    if (ImGui::IsItemDeactivated())
        EndContinuous(geom, what);
}

void DrawInspector()
{
    const engine::GeomInfo* info = g_selected ? FindInfo(g_selected) : nullptr;
    if (!info || !IsLive(g_selected)) {
        ImGui::TextWrapped("Nothing selected.");
        ImGui::TextDisabled("Click an object in the world, or pick it\nin the Scene tab. Models: place new ones.");
        return;
    }
    engine::ZGEOM* geom = info->geom;
    ImGui::Text("%s", info->name.empty() ? "<unnamed>" : info->name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", info->className.c_str());
    if (ImGui::TreeNode("details")) {  // the engine's view: for scripts (h2.find) and debugging
        ImGui::TextWrapped("class: %s", engine::ClassChain(geom).c_str());
        ImGui::TextWrapped("path: %s", info->path.c_str());
        if (ImGui::SmallButton("copy path"))
            ImGui::SetClipboardText(info->path.c_str());
        ImGui::TextDisabled("geom %p  node %p  flags %08X", static_cast<void*>(geom), static_cast<void*>(info->node),
                            info->node->flags);
        ImGui::TreePop();
    }
    if (info->parent) {
        const engine::GeomInfo* parent = FindInfo(info->parent->geom);
        ImGui::Text("parent: %s", parent ? parent->name.c_str() : "?");
        if (parent) {
            ImGui::SameLine();
            if (ImGui::SmallButton("select parent")) {
                g_selected = parent->geom;
                g_scrollToSelected = true;
            }
        }
    }
    ImGui::Text("children: %d", int(info->children.size()));
    ImGui::SameLine();
    if (ImGui::SmallButton("show in tree"))
        g_scrollToSelected = true;
    ImGui::Separator();

    Transform t = GetTransform(geom);
    bool actor = engine::IsA(geom, "ZActor");
    if (actor)
        ImGui::TextDisabled("character: moving it teleports it, its behaviour carries on from there");
    if (ImGui::DragFloat3("position", t.pos, 1.0f, 0, 0, "%.1f"))
        SetTransform(geom, t);
    TrackContinuous(geom, "move");
    float e[3];
    MatrixToEuler(t.mat, e[0], e[1], e[2]);
    if (ImGui::DragFloat3("yaw/pitch/roll", e, 0.5f, 0, 0, "%.1f")) {
        EulerToMatrix(e[0], e[1], e[2], t.mat);
        SetTransform(geom, t);
    }
    TrackContinuous(geom, "rotate");

    bool hidden = IsHidden(geom);
    if (ImGui::Checkbox("hidden (H)", &hidden)) {
        ObjectState before = GetState(geom), after = before;
        after.hidden = hidden;
        Change(geom, before, after, hidden ? "hide" : "show");
    }
    ImGui::SameLine();
    if (ImGui::Button("Fly camera here (F)")) {
        FlyCameraTo(t.pos);
    }
    ImGui::BeginDisabled(actor || InActor(geom) || !engine::IsPlainModel(engine::MeshOf(geom)));  // (a body: NPCs tab)
    if (ImGui::Button("Duplicate (Ctrl+D)"))
        DuplicateSelection();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(Placing());
    if (ImGui::Button("Grab (G)"))
        BeginGrab(geom);
    ImGui::EndDisabled();
    if (const SpawnedObject* spawned = FindSpawned(geom)) {
        ImGui::SameLine();
        if (ImGui::Button("Delete (Del)"))
            DeleteSelection();
        if (spawned->point) {
            ImGui::TextDisabled("point made by the level (invisible in the game)");
            // its name: routes, goals and scripts (h2.find) name it
            static char name[48];
            static engine::ZGEOM* nameOf;
            static std::string renameError;
            if (nameOf != geom) {
                snprintf(name, sizeof(name), "%s", spawned->id.c_str());
                nameOf = geom;
                renameError.clear();
            }
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
            bool enter = ImGui::InputText("##pointname", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            ImGui::BeginDisabled(spawned->id == name);
            if (ImGui::Button("Rename") || (enter && spawned->id != name)) {
                std::string err;
                if (engine::ZGEOM* renamed = RenamePoint(geom, name, &err)) {
                    Select(renamed);
                    nameOf = renamed;
                    renameError.clear();
                    ImGui::EndDisabled();
                    return;  // (geom was made again: the rest next frame)
                } else {
                    renameError = err;
                }
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("A name of its own, e.g. Garage (letters, digits, _ and -).\n"
                                  "The level's routes, goals and characters' settings follow it;\n"
                                  "scripts find it by the new name: h2.find(\"Garage\").");
            if (!renameError.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", renameError.c_str());
            // its icon on the in-game map (M): made with the map
            const auto& icons = engine::MapIconNames();
            std::string mission = MissionMapIcon(spawned->id);  // (the exit's, when it has none of its own)
            std::string none = mission.empty() ? "none" : "the mission's (" + mission.substr(5) + ")";
            std::string shown = spawned->mapIcon.empty() ? none : spawned->mapIcon.substr(5);  // ("Icon_")
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
            if (ImGui::BeginCombo("On the map (M)", shown.c_str())) {
                if (ImGui::Selectable(none.c_str(), spawned->mapIcon.empty()))
                    SetPointMapIcon(geom, "");
                for (const std::string& icon : icons)
                    if (ImGui::Selectable(icon.substr(5).c_str(), spawned->mapIcon == icon))
                        SetPointMapIcon(geom, icon);
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("An icon of the game's map legend at this point, on the floor the map\n"
                                  "shows for its room. Shown from the map's next load (Test).");
        } else if (spawned->item)
            ImGui::TextDisabled("item placed by the level (%s)", spawned->sourcePath.c_str());
        else
            ImGui::TextDisabled("level object, copy of %s", spawned->sourcePath.c_str());
    }
    DrawMissionForSelection(geom);
    DrawVehicleForSelection(geom);
}

void DrawSelectionMarker()
{
    if (!g_selected || !IsLive(g_selected))
        return;
    float pos[3], sx, sy;
    engine::WorldPosition(g_selected, pos);
    if (!WorldToScreen(pos, sx, sy))
        return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImU32 col = IM_COL32(255, 200, 40, 255);
    dl->AddCircle(ImVec2(sx, sy), 10.0f, col, 16, 2.0f);
    const engine::GeomInfo* info = FindInfo(g_selected);
    if (info)
        dl->AddText(ImVec2(sx + 14, sy - 22), col, info->name.c_str());
}

void OnScene(const std::string&) { g_selected = nullptr; }

} // namespace

engine::ZGEOM* Selected() { return g_selected; }

void Select(engine::ZGEOM* geom)
{
    g_selected = geom;
    g_scrollToSelected = geom != nullptr;
}

bool WorldToScreen(const float world[3], float& sx, float& sy)
{
    game::ZCAMERA* cam = game::MainCamera();
    if (!cam || RenderWidth() <= 0)
        return false;
    float m[9], c[3];
    game::GetWorldTransform(cam, m, c);
    float v[3] = {world[0] - c[0], world[1] - c[1], world[2] - c[2]};
    float z = v[0] * m[0] + v[1] * m[1] + v[2] * m[2];  // forward
    if (z < 1.0f)
        return false;
    float y = v[0] * m[3] + v[1] * m[4] + v[2] * m[5];  // up
    float x = v[0] * m[6] + v[1] * m[7] + v[2] * m[8];  // right
    float w = float(RenderWidth()), h = float(RenderHeight());
    float tanH = CameraTanHalfFov(cam);
    float tanV = tanH * h / w;
    sx = (x / z / tanH * 0.5f + 0.5f) * w;
    sy = (0.5f - y / z / tanV * 0.5f) * h;
    return true;
}

void InstallScenePanels()
{
    overlay::AddPanel({"Scene", &g_sceneVisible, DrawScenePanel, overlay::Dock::Left});
    overlay::AddPanel({"Inspector", &g_inspectorVisible, DrawInspector, overlay::Dock::Right});
    static bool always = true;
    overlay::AddPanel({nullptr, &always, DrawSelectionMarker});
    OnScenePushed(OnScene);
}

} // namespace h2::editor
