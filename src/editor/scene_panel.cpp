#include "scene_panel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>

#include "imgui.h"

#include "../display.h"
#include "../engine/geoms.h"
#include "../fov.h"
#include "../game.h"
#include "../log.h"
#include "../overlay/overlay.h"
#include "../scene.h"
#include "freecam.h"

namespace h2::editor {

namespace {

engine::SceneSnapshot g_snap;
double g_lastRefresh = -1e9;
bool g_sceneVisible = true;
bool g_inspectorVisible = true;
bool g_autoRefresh = true;
char g_filter[128];
engine::ZGEOM* g_selected;
std::vector<int> g_matches;
std::string g_matchFilter;

void Refresh()
{
    g_snap = engine::TakeSnapshot();
    g_lastRefresh = NowMs();
    g_matchFilter = "\x01";  // force re-filter
    bool stillThere = false;
    for (auto& g : g_snap.geoms)
        stillThere |= g.geom == g_selected;
    if (!stillThere)
        g_selected = nullptr;
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

const engine::GeomInfo* FindInfo(engine::ZGEOM* geom)
{
    for (auto& g : g_snap.geoms)
        if (g.geom == geom)
            return &g;
    return nullptr;
}

void DrawTreeNode(int i)
{
    const engine::GeomInfo& g = g_snap.geoms[i];
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (g.children.empty())
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (g.geom == g_selected)
        flags |= ImGuiTreeNodeFlags_Selected;
    bool open = ImGui::TreeNodeEx(static_cast<void*>(g.node), flags, "%s  (%s)",
                                  g.name.empty() ? "<unnamed>" : g.name.c_str(), g.className.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        g_selected = g.geom;
    if (open && !g.children.empty()) {
        for (int c : g.children)
            DrawTreeNode(c);
        ImGui::TreePop();
    }
}

void DrawScenePanel()
{
    ImGui::SetNextWindowSize(ImVec2(520, 700), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 540, 40), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Scene", &g_sceneVisible)) {
        ImGui::End();
        return;
    }
    if (g_autoRefresh && NowMs() - g_lastRefresh > 2000)
        Refresh();
    if (ImGui::Button("Refresh"))
        Refresh();
    ImGui::SameLine();
    ImGui::Checkbox("auto", &g_autoRefresh);
    ImGui::SameLine();
    ImGui::TextDisabled("%d objects", int(g_snap.geoms.size()));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "filter by name or class", g_filter, sizeof(g_filter));

    if (ImGui::BeginChild("tree", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        std::string filter = Lower(g_filter);
        if (filter.empty()) {
            for (int r : g_snap.roots)
                DrawTreeNode(r);
        } else {
            if (filter != g_matchFilter) {
                g_matchFilter = filter;
                g_matches.clear();
                for (int i = 0; i < int(g_snap.geoms.size()); ++i) {
                    const auto& g = g_snap.geoms[i];
                    if (Lower(g.name).find(filter) != std::string::npos ||
                        Lower(g.className).find(filter) != std::string::npos)
                        g_matches.push_back(i);
                }
            }
            ImGuiListClipper clipper;
            clipper.Begin(int(g_matches.size()));
            while (clipper.Step()) {
                for (int k = clipper.DisplayStart; k < clipper.DisplayEnd; ++k) {
                    const auto& g = g_snap.geoms[g_matches[k]];
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
    ImGui::End();
}

void DrawInspector()
{
    ImGui::SetNextWindowSize(ImVec2(520, 320), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 540, 760), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Inspector", &g_inspectorVisible)) {
        ImGui::End();
        return;
    }
    const engine::GeomInfo* info = g_selected ? FindInfo(g_selected) : nullptr;
    if (!info) {
        ImGui::TextDisabled("select an object in the Scene panel");
        ImGui::End();
        return;
    }
    ImGui::Text("%s", info->name.empty() ? "<unnamed>" : info->name.c_str());
    ImGui::TextWrapped("class: %s", engine::ClassChain(info->geom).c_str());
    ImGui::TextDisabled("geom %p  node %p  flags %08X", static_cast<void*>(info->geom),
                        static_cast<void*>(info->node), info->node->flags);
    if (info->parent) {
        const engine::GeomInfo* parent = FindInfo(info->parent->geom);
        ImGui::Text("parent: %s", parent ? parent->name.c_str() : "?");
        if (parent) {
            ImGui::SameLine();
            if (ImGui::SmallButton("select parent"))
                g_selected = parent->geom;
        }
    }
    ImGui::Text("children: %d", int(info->children.size()));
    ImGui::Separator();

    float m[9], pos[3];
    game::GetWorldTransform(info->geom, m, pos);
    float edited[3] = {pos[0], pos[1], pos[2]};
    if (ImGui::DragFloat3("world position", edited, 1.0f, 0, 0, "%.1f"))
        game::SetWorldTransform(info->geom, m, edited);
    ImGui::TextDisabled("forward %.2f %.2f %.2f", m[0], m[1], m[2]);
    ImGui::TextDisabled("up      %.2f %.2f %.2f", m[3], m[4], m[5]);
    if (ImGui::Button("Fly camera here")) {
        // stand 3 m back and 1.5 m up, looking at the object
        float cam[3] = {pos[0] - m[0] * 300.0f, pos[1] + 150.0f, pos[2] - m[2] * 300.0f};
        SetFreeCamEnabled(true);
        SetFreeCamPosition(cam);
    }
    ImGui::End();
}

void DrawSelectionMarker()
{
    if (!g_selected || !FindInfo(g_selected))
        return;
    float m[9], pos[3], sx, sy;
    game::GetWorldTransform(g_selected, m, pos);
    if (!WorldToScreen(pos, sx, sy))
        return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImU32 col = IM_COL32(255, 200, 40, 255);
    dl->AddCircle(ImVec2(sx, sy), 10.0f, col, 16, 2.0f);
    dl->AddLine(ImVec2(sx - 16, sy), ImVec2(sx - 6, sy), col, 2.0f);
    dl->AddLine(ImVec2(sx + 6, sy), ImVec2(sx + 16, sy), col, 2.0f);
    const engine::GeomInfo* info = FindInfo(g_selected);
    dl->AddText(ImVec2(sx + 14, sy - 22), col, info->name.c_str());
}

void DrawAll()
{
    DrawScenePanel();
}

void OnScene(const std::string&)
{
    g_selected = nullptr;
    g_snap = {};
    g_lastRefresh = -1e9;
}

} // namespace

engine::ZGEOM* Selected() { return g_selected; }
void Select(engine::ZGEOM* geom) { g_selected = geom; }

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
    float tanH = std::tan(cam->fov * 0.5f) * HorizontalTanScale();
    float tanV = tanH * h / w;
    sx = (x / z / tanH * 0.5f + 0.5f) * w;
    sy = (0.5f - y / z / tanV * 0.5f) * h;
    return true;
}

void InstallScenePanels()
{
    overlay::AddPanel({"Scene", &g_sceneVisible, DrawAll});
    overlay::AddPanel({"Inspector", &g_inspectorVisible, DrawInspector});
    static bool always = true;
    overlay::AddPanel({nullptr, &always, DrawSelectionMarker});
    OnScenePushed(OnScene);
}

} // namespace h2::editor
