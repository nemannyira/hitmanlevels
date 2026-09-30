#include "models_panel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"

#include "../engine/spawn.h"
#include "../game.h"
#include "../overlay/overlay.h"
#include "../scene.h"
#include "../display.h"
#include "freecam.h"
#include "highlight.h"
#include "level.h"
#include "model.h"
#include "placement.h"
#include "scene_panel.h"

namespace h2::editor {

namespace {

struct ModelEntry {
    uint32_t mesh;
    std::string name;        // display name: object name without its numbering
    std::string className;   // class of the object it is taken from
    std::string sourcePath;  // an object of the scene using the mesh
    float radius = 0;
    int uses = 0;
};

// sound/sight helper boxes and light volumes: not models to place
constexpr uint32_t kHelperFlags = 0x2000 | 0x10000;

bool g_visible = true;
char g_filter[128];
std::vector<ModelEntry> g_models;
std::string g_builtFor;  // scene the list was built for
std::string g_message;

// Thumbnail of the hovered model: a clone of it stands in front of the camera
// (so the engine draws it) and its draws are rendered into a turning picture.
std::string g_hoverPath;       // model hovered this frame
std::string g_hoverCandidate;  // hovered since g_hoverSince
double g_hoverSince, g_lastHover;
std::string g_thumbPath;       // model the clone is made of
engine::ZGEOM* g_thumb;
engine::ZGEOM* g_viewRoom;     // room of what is in the middle of the screen
double g_viewRoomTime = -1e9;
bool g_viewRoomPending;
constexpr double kHoverDelay = 0.15, kHoverLinger = 0.5;

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// "Wall_01b_05" -> "Wall_01b", "Tree_01_02" -> "Tree_01": the trailing
// instance number of the level designers' naming.
std::string BaseName(const std::string& name)
{
    std::string s = name;
    size_t us = s.rfind('_');
    if (us != std::string::npos && us + 1 < s.size() && us > 0 &&
        std::all_of(s.begin() + us + 1, s.end(), [](unsigned char c) { return std::isdigit(c); }))
        s.resize(us);
    return s.empty() ? name : s;
}

void Build()
{
    const engine::SceneSnapshot& snap = SceneModel(0);
    std::map<uint32_t, ModelEntry> byMesh;
    for (const engine::GeomInfo& g : snap.geoms) {
        if (!IsLive(g.geom) || IsSpawned(g.geom) || InActor(g.geom))
            continue;
        uint32_t mesh = engine::MeshOf(g.geom);
        if (!mesh || (g.node->flags & kHelperFlags))
            continue;
        ModelEntry& e = byMesh[mesh];
        if (e.uses++ == 0) {
            e.mesh = mesh;
            e.name = BaseName(g.name);
            e.className = g.className;
            e.sourcePath = g.path;
            e.radius = g.node->boundRadius;
        }
    }
    g_models.clear();
    for (auto& [mesh, e] : byMesh)
        g_models.push_back(e);
    std::sort(g_models.begin(), g_models.end(), [](const ModelEntry& a, const ModelEntry& b) {
        std::string la = Lower(a.name), lb = Lower(b.name);
        return la != lb ? la < lb : a.mesh < b.mesh;
    });
    g_builtFor = CurrentScene();
}

void DestroyThumb()
{
    SetThumbnailGeom(nullptr);
    if (g_thumb && IsSpawned(g_thumb))
        DestroySpawned(g_thumb);
    g_thumb = nullptr;
    g_thumbPath.clear();
}

void PlaceThumb()
{
    game::ZCAMERA* cam = game::MainCamera();
    if (!cam || !g_thumb || !IsLive(g_thumb))
        return;
    float m[9], c[3];
    game::GetWorldTransform(cam, m, c);
    Transform t = GetTransform(g_thumb);
    for (int i = 0; i < 3; ++i)
        t.pos[i] = c[i] + m[i] * 300.0f;
    Transform cur = GetTransform(g_thumb);
    if (std::fabs(cur.pos[0] - t.pos[0]) + std::fabs(cur.pos[1] - t.pos[1]) + std::fabs(cur.pos[2] - t.pos[2]) > 1.0f)
        SetTransform(g_thumb, t);
}

// Every frame the overlay is open (also when the Models tab is not shown).
void ThumbTick()
{
    double now = ImGui::GetTime();
    std::string hovered = g_hoverPath;
    g_hoverPath.clear();
    if (!hovered.empty()) {
        g_lastHover = now;
        if (hovered != g_hoverCandidate) {
            g_hoverCandidate = hovered;
            g_hoverSince = now;
        }
        // keep track of the room in view: the clone must be in a room the engine draws
        if (!g_viewRoomPending && now - g_viewRoomTime > 1.0) {
            g_viewRoomPending = true;
            RequestPick(RenderWidth() * 0.5f, RenderHeight() * 0.5f, [](engine::ZGEOM* hit, const float*) {
                g_viewRoomPending = false;
                g_viewRoomTime = ImGui::GetTime();
                if (engine::ZGEOM* room = hit ? RoomOf(hit) : nullptr)
                    g_viewRoom = room;
            });
        }
        if (now - g_viewRoomTime > 3.0)
            g_viewRoomPending = false;  // the request was taken over
        if (now - g_hoverSince >= kHoverDelay && g_thumbPath != hovered && !Placing()) {
            DestroyThumb();
            if (engine::ZGEOM* source = Resolve(hovered)) {
                std::string err;
                engine::ZGEOM* room = g_viewRoom && IsLive(g_viewRoom) ? g_viewRoom : nullptr;
                g_thumb = SpawnClone(source, "EditorThumb", &err, room);
                g_thumbPath = hovered;
                if (g_thumb) {
                    PlaceThumb();
                    SetThumbnailGeom(g_thumb);
                }
            }
        }
    } else if (g_thumb && now - g_lastHover > kHoverLinger) {
        DestroyThumb();
        ReleaseThumbnail();
    }
    if (g_thumb) {
        if (!IsLive(g_thumb))
            DestroyThumb();
        else
            PlaceThumb();
    }
}

void DrawTooltip(const ModelEntry& e)
{
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(e.name.c_str());
    ImGui::TextDisabled("%s, %.1f m, used %d times", e.className.c_str(), e.radius * 2.0f / 100.0f, e.uses);
    float size = 220.0f * ImGui::GetIO().FontGlobalScale * (ImGui::GetFontSize() / 16.0f);
    void* tex = g_thumbPath == e.sourcePath ? ThumbnailTexture() : nullptr;
    if (tex)
        ImGui::Image(reinterpret_cast<ImTextureID>(tex), ImVec2(size, size));
    else
        ImGui::Dummy(ImVec2(size, size));  // keeps the tooltip from jumping when the picture arrives
    ImGui::TextDisabled("click to place");
    ImGui::EndTooltip();
}

void DrawPanel()
{
    if (CurrentScene().empty()) {
        ImGui::TextDisabled("no scene loaded");
        return;
    }
    if (g_builtFor != CurrentScene() || g_models.empty())
        Build();
    if (ImGui::Button("Refresh"))
        Build();
    ImGui::SameLine();
    ImGui::TextDisabled("%d models in %s", int(g_models.size()), g_builtFor.c_str());
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "filter by name or class", g_filter, sizeof(g_filter));
    if (Placing()) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "placing: move the mouse over the world, click");
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel (Esc)"))
            CancelPlacement();
    } else {
        ImGui::TextDisabled("click a model, then click in the world to place it");
    }

    std::string filter = Lower(g_filter);
    std::vector<int> shown;
    for (int i = 0; i < int(g_models.size()); ++i)
        if (filter.empty() || Lower(g_models[i].name).find(filter) != std::string::npos ||
            Lower(g_models[i].className).find(filter) != std::string::npos)
            shown.push_back(i);

    if (ImGui::BeginTable("models", 4,
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_Resizable,
                          ImVec2(0, -ImGui::GetTextLineHeightWithSpacing() * 1.5f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("model", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("uses", ImGuiTableColumnFlags_WidthFixed, 36);
        ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, 56);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(int(shown.size()));
        while (clipper.Step()) {
            for (int k = clipper.DisplayStart; k < clipper.DisplayEnd; ++k) {
                const ModelEntry& e = g_models[shown[k]];
                ImGui::PushID(shown[k]);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool current = PlacementSource() == e.sourcePath;
                if (ImGui::Selectable(e.name.c_str(), current, ImGuiSelectableFlags_SpanAllColumns |
                                                                   ImGuiSelectableFlags_AllowOverlap)) {
                    if (current)
                        CancelPlacement();
                    else
                        BeginPlacement(e.sourcePath, e.name);
                }
                if (ImGui::IsItemHovered() && !Placing()) {
                    g_hoverPath = e.sourcePath;
                    DrawTooltip(e);
                }
                ImGui::TableNextColumn();
                ImGui::Text("%d", e.uses);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f m", e.radius * 2.0f / 100.0f);  // bounding sphere diameter
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Find")) {
                    if (engine::ZGEOM* g = Resolve(e.sourcePath)) {
                        Select(g);
                        Transform t = GetTransform(g);
                        float cam[3] = {t.pos[0] - t.mat[0] * 300.0f, t.pos[1] + 150.0f, t.pos[2] - t.mat[2] * 300.0f};
                        SetFreeCamEnabled(true);
                        SetFreeCamPosition(cam);
                    }
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    if (!g_message.empty())
        ImGui::TextDisabled("%s", g_message.c_str());
}

} // namespace

void InstallModelsPanel()
{
    overlay::AddPanel({"Models", &g_visible, DrawPanel, overlay::Dock::Left});
    static bool always = true;
    overlay::AddPanel({nullptr, &always, ThumbTick});
    OnScenePushed([](const std::string&) {
        g_models.clear();
        g_builtFor.clear();
        g_thumb = nullptr;  // destroyed with the editor's objects when the scene was left
        g_thumbPath.clear();
        g_viewRoom = nullptr;
        SetThumbnailGeom(nullptr);
    });
}

} // namespace h2::editor
