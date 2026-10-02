#include "models_panel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"

#include "../engine/imports.h"
#include "../engine/spawn.h"
#include "../game.h"
#include "../overlay/overlay.h"
#include "../scene.h"
#include "../display.h"
#include "freecam.h"
#include "highlight.h"
#include "level.h"
#include "model.h"
#include "model_catalog.h"
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
    Category category = Category::Other;
    std::string family;
    std::string templatePath;  // working item: its template (sourcePath = its largest visible part)
    std::string level;         // a model of another level: that level
    Category autoCategory = Category::Other;  // before the user's correction

    // what the thumbnail and the preview are made of
    const std::string& Key() const { return templatePath.empty() ? sourcePath : templatePath; }
};

// sound/sight helper boxes and light volumes: not models to place
constexpr uint32_t kHelperFlags = 0x2000 | 0x10000;

bool g_visible = true;
char g_filter[128];
bool g_showHelpers = false;
std::vector<ModelEntry> g_models;  // sorted by category, family, name
std::string g_builtFor;  // scene the list was built for
std::string g_message;

// Thumbnail of the hovered model: a clone of it stands in front of the camera
// (so the engine draws it) and its draws are rendered into a turning picture.
std::string g_hoverPath;       // model hovered this frame (ModelEntry::Key)
std::string g_hoverCandidate;  // hovered since g_hoverSince
double g_hoverSince, g_lastHover;
std::string g_thumbPath;       // model the clone is made of
PartClones g_thumb;
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

// "Truck_Body#0" -> "Truck_Body", "LampPost01_16" -> "LampPost01"
std::string ShownName(const std::string& name)
{
    return BaseName(name.substr(0, name.find('#')));
}

void ApplyOverride(ModelEntry& e)
{
    e.category = e.autoCategory;
    Category c;
    if (CategoryOverride(e.name, &c) || CategoryOverride("family:" + e.family, &c))
        e.category = c;
}

void Sort()
{
    std::sort(g_models.begin(), g_models.end(), [](const ModelEntry& a, const ModelEntry& b) {
        if (a.category != b.category)
            return a.category < b.category;
        std::string fa = Lower(a.family), fb = Lower(b.family);
        if (fa != fb)
            return fa < fb;
        std::string la = Lower(a.name), lb = Lower(b.name);
        return la != lb ? la < lb : a.mesh < b.mesh;
    });
}

void Build()
{
    const engine::SceneSnapshot& snap = SceneModel(0);
    g_models.clear();
    // item templates: placed as working items, shown with all their visible parts
    // (collected first: VisibleParts / PathOf may refresh the snapshot)
    struct Template {
        engine::ZGEOM* geom;
        std::string name, className, path;
    };
    std::vector<Template> templates;
    for (const engine::GeomInfo& g : snap.geoms)
        if (IsLive(g.geom) && !IsSpawned(g.geom) && engine::IsA(g.geom, "ZItemTemplate"))
            templates.push_back({g.geom, g.name, g.className, g.path});
    std::map<uint32_t, bool> templateMeshes;
    for (const Template& g : templates) {
        if (!IsLive(g.geom))
            continue;
        std::vector<engine::ZGEOM*> parts = VisibleParts(g.geom);
        if (parts.empty())
            continue;
        for (engine::ZGEOM* part : parts)
            templateMeshes[engine::MeshOf(part)] = true;
        ModelEntry e;
        e.mesh = engine::MeshOf(parts[0]);
        e.name = g.name;
        e.className = g.className;
        e.sourcePath = PathOf(parts[0]);
        e.templatePath = g.path;
        e.radius = parts[0]->node->boundRadius;
        e.uses = 1;
        e.autoCategory = Category::Pickup;
        e.family = FamilyOf(e.name);
        ApplyOverride(e);
        g_models.push_back(e);
    }
    std::map<uint32_t, ModelEntry> byMesh;
    for (const engine::GeomInfo& g : SceneModel().geoms) {  // nothing below refreshes it
        std::string level = ImportLevelOf(g.path);
        if (!IsLive(g.geom) || (IsSpawned(g.geom) && level.empty()) || InActor(g.geom))
            continue;
        uint32_t mesh = engine::MeshOf(g.geom);
        if (!mesh || (g.node->flags & kHelperFlags) || templateMeshes.count(mesh))
            continue;  // models of item templates: placed as working items (above)
        ModelEntry& e = byMesh[mesh];
        if (e.uses++ == 0) {
            e.mesh = mesh;
            e.name = level.empty() ? BaseName(g.name) : ShownName(g.name.substr(0, g.name.find('.')));
            e.level = level;
            e.className = g.className;
            e.sourcePath = g.path;
            e.radius = g.node->boundRadius;
            // menus, HUD and the map screen are not in any room of the level
            e.autoCategory = RoomOf(g.geom) ? Categorize(g.name, g.className) : Category::Interface;
            e.family = FamilyOf(e.name);
            ApplyOverride(e);
        }
    }
    for (auto& [mesh, e] : byMesh)
        g_models.push_back(e);
    Sort();
    g_builtFor = CurrentScene();
}

void DestroyThumb()
{
    SetThumbnailGeoms({});
    g_thumb.Destroy();
    g_thumbPath.clear();
}

void PlaceThumb()
{
    game::ZCAMERA* cam = game::MainCamera();
    if (!cam || !g_thumb.Live())
        return;
    float m[9], c[3];
    game::GetWorldTransform(cam, m, c);
    Transform t = GetTransform(g_thumb.Main());
    for (int i = 0; i < 3; ++i)
        t.pos[i] = c[i] + m[i] * 300.0f;
    Transform cur = GetTransform(g_thumb.Main());
    if (std::fabs(cur.pos[0] - t.pos[0]) + std::fabs(cur.pos[1] - t.pos[1]) + std::fabs(cur.pos[2] - t.pos[2]) > 1.0f)
        g_thumb.Place(t);
}

// What a model of the list is made of: an item template's visible parts, or
// the scene object using the model.
std::vector<engine::ZGEOM*> ThumbSources(const std::string& key)
{
    engine::ZGEOM* g = Resolve(key);
    if (!g)
        return {};
    if (engine::IsA(g, "ZItemTemplate"))
        return VisibleParts(g);
    return {g};
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
            RequestPick(RenderWidth() * 0.5f, RenderHeight() * 0.5f, [](engine::ZGEOM* hit, const float*, const float*) {
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
            std::vector<engine::ZGEOM*> sources = ThumbSources(hovered);
            g_thumbPath = hovered;
            std::string err;
            engine::ZGEOM* room = g_viewRoom && IsLive(g_viewRoom) ? g_viewRoom : nullptr;
            if (!sources.empty() && g_thumb.Spawn(sources, "EditorThumb", &err, room)) {
                PlaceThumb();
                SetThumbnailGeoms(g_thumb.parts);
            }
        }
    } else if (!g_thumb.parts.empty() && now - g_lastHover > kHoverLinger) {
        DestroyThumb();
        ReleaseThumbnail();
    }
    if (!g_thumb.parts.empty()) {
        if (!g_thumb.Live())
            DestroyThumb();
        else
            PlaceThumb();
    }
}

void DrawTooltip(const ModelEntry& e)
{
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(e.name.c_str());
    if (!e.level.empty())
        ImGui::TextDisabled("from %s, %.1f m", e.level.c_str(), e.radius * 2.0f / 100.0f);
    else
        ImGui::TextDisabled("%s, %.1f m, used %d times", e.className.c_str(), e.radius * 2.0f / 100.0f, e.uses);
    float size = 220.0f * ImGui::GetIO().FontGlobalScale * (ImGui::GetFontSize() / 16.0f);
    void* tex = g_thumbPath == e.Key() ? ThumbnailTexture() : nullptr;
    if (tex)
        ImGui::Image(reinterpret_cast<ImTextureID>(tex), ImVec2(size, size));
    else
        ImGui::Dummy(ImVec2(size, size));  // keeps the tooltip from jumping when the picture arrives
    ImGui::TextDisabled("click to place");
    ImGui::EndTooltip();
}

void FindInScene(const ModelEntry& e)
{
    if (engine::ZGEOM* g = Resolve(e.sourcePath)) {
        Select(g);
        Transform t = GetTransform(g);
        float cam[3] = {t.pos[0] - t.mat[0] * 300.0f, t.pos[1] + 150.0f, t.pos[2] - t.mat[2] * 300.0f};
        SetFreeCamEnabled(true);
        SetFreeCamPosition(cam);
    }
}

bool g_resort;  // a category was changed: sort again after drawing

// Submenu putting the model (key = its name) or its family ("family:<name>")
// into another category; remembered in Mods\model_categories.json.
void CategoryMenu(const char* label, const std::string& key, const ModelEntry& e)
{
    if (!ImGui::BeginMenu(label))
        return;
    Category current;
    bool overridden = CategoryOverride(key, &current);
    for (int c = 0; c < int(Category::Count); ++c) {
        Category cat = Category(c);
        if (cat == Category::Pickup && e.templatePath.empty())
            continue;  // only item templates work as pickups
        if (ImGui::MenuItem(CategoryName(cat), nullptr, overridden ? current == cat : e.category == cat)) {
            SetCategoryOverride(key, cat == e.autoCategory ? Category::Count : cat);
            g_resort = true;
        }
    }
    if (overridden) {
        ImGui::Separator();
        if (ImGui::MenuItem("Automatic")) {
            SetCategoryOverride(key, Category::Count);
            g_resort = true;
        }
    }
    ImGui::EndMenu();
}

void DrawRow(const ModelEntry& e, int index)
{
    ImGui::PushID(index);
    bool current = PlacementSource() == e.sourcePath;
    char size[48];
    if (e.level.empty())
        snprintf(size, sizeof(size), "%.1f m", e.radius * 2.0f / 100.0f);  // bounding sphere diameter
    else
        snprintf(size, sizeof(size), "%s  %.1f m", e.level.c_str(), e.radius * 2.0f / 100.0f);
    float sizeW = ImGui::CalcTextSize(size).x;
    if (ImGui::Selectable(e.name.c_str(), current, ImGuiSelectableFlags_AllowOverlap)) {
        if (current)
            CancelPlacement();
        else
            BeginPlacement(e.sourcePath, e.name, e.templatePath);
    }
    if (ImGui::IsItemHovered() && !Placing()) {
        g_hoverPath = e.Key();
        DrawTooltip(e);
    }
    if (ImGui::BeginPopupContextItem("row")) {
        if (ImGui::MenuItem("Place"))
            BeginPlacement(e.sourcePath, e.name, e.templatePath);
        if (ImGui::MenuItem("Find in the scene"))
            FindInScene(e);
        CategoryMenu("Move to category", e.name, e);
        if (!e.family.empty() && e.family != e.name)
            CategoryMenu(("Move every " + e.family + " to").c_str(), "family:" + e.family, e);
        ImGui::TextDisabled("%s, used %d times", e.className.c_str(), e.uses);
        ImGui::EndPopup();
    }
    ImGui::SameLine(ImGui::GetContentRegionMax().x - sizeW);
    ImGui::TextDisabled("%s", size);
    ImGui::PopID();
}

// ---- models of other levels ----

bool g_otherOpen;
int g_otherLevel = -1;
char g_otherFilter[128];
std::string g_otherMessage;

void DrawOtherLevels()
{
    if (!g_otherOpen)
        return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30, io.DisplaySize.y * 0.7f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Models of other levels", &g_otherOpen)) {
        ImGui::End();
        return;
    }
    const std::vector<std::string>& levels = engine::GameLevels();
    std::string here = CurrentLevel().baseScene.empty() ? CurrentScene() : CurrentLevel().baseScene;
    auto isHere = [&](int i) { return here.find(levels[i] + "\\") == 0; };
    if (g_otherLevel < 0 || g_otherLevel >= int(levels.size()) || isHere(g_otherLevel)) {
        g_otherLevel = -1;
        for (int i = 0; i < int(levels.size()) && g_otherLevel < 0; ++i)
            if (!isHere(i))
                g_otherLevel = i;
    }
    ImGui::TextWrapped("Tick the models this level should have. The map is loaded again to get them; then they are "
                       "in the Models list (marked with their level) and are placed like the others.");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    if (ImGui::BeginCombo("level", g_otherLevel >= 0 ? levels[g_otherLevel].c_str() : "-")) {
        for (int i = 0; i < int(levels.size()); ++i)
            if (!isHere(i) && ImGui::Selectable(levels[i].c_str(), i == g_otherLevel))
                g_otherLevel = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##ofilter", "filter", g_otherFilter, sizeof(g_otherFilter));
    if (g_otherLevel < 0) {
        ImGui::End();
        return;
    }
    const std::string& level = levels[g_otherLevel];
    const std::vector<engine::LevelModel>& models = engine::LevelModels(level);
    engine::ZGEOM* known = Resolve("ROOT");
    struct Row {
        const engine::LevelModel* m;
        Category cat;
    };
    std::vector<Row> rows;
    std::string filter = Lower(g_otherFilter);
    for (const engine::LevelModel& m : models) {
        if (!filter.empty() && Lower(m.name).find(filter) == std::string::npos &&
            Lower(m.group).find(filter) == std::string::npos)
            continue;
        std::string chain = engine::ClassChainOfId(m.classId, known);
        Category cat = Categorize(m.name, chain.substr(0, chain.find(' ')));
        if (cat == Category::Helpers || cat == Category::Interface)
            continue;  // invisible technical objects, menus
        rows.push_back({&m, cat});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.cat != b.cat ? a.cat < b.cat : Lower(a.m->name) < Lower(b.m->name);
    });
    float footer = ImGui::GetFrameHeightWithSpacing() * 2 + ImGui::GetTextLineHeightWithSpacing();
    if (ImGui::BeginChild("olist", ImVec2(0, -footer), ImGuiChildFlags_Borders)) {
        if (models.empty())
            ImGui::TextDisabled("no models (the level could not be read)");
        size_t i = 0;
        while (i < rows.size()) {
            Category cat = rows[i].cat;
            size_t end = i;
            while (end < rows.size() && rows[end].cat == cat)
                ++end;
            if (!filter.empty())
                ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            char header[96];
            snprintf(header, sizeof(header), "%s  (%d)###ocat%d", CategoryName(cat), int(end - i), int(cat));
            if (ImGui::CollapsingHeader(header)) {
                for (size_t k = i; k < end; ++k) {
                    const engine::LevelModel& m = *rows[k].m;
                    ImGui::PushID(int(m.mesh));
                    bool on = IsImported(level, m.mesh);
                    std::string label = ShownName(m.name);
                    if (!m.pieces.empty())
                        label += "  (" + std::to_string(m.pieces.size()) + " parts)";
                    if (ImGui::Checkbox(label.c_str(), &on)) {
                        int uses = on ? 0 : ImportUses(level, m.mesh);
                        if (uses)
                            g_otherMessage = m.name + " is used by " + std::to_string(uses) +
                                             " placed objects: remove them first";
                        else
                            SetImported(level, m.mesh, m.name, on);
                    }
                    if (ImGui::IsItemHovered()) {
                        if (m.pieces.empty())
                            ImGui::SetTooltip("%s\nin %s, used %d times there, %d materials", m.name.c_str(),
                                              m.group.c_str(), m.uses, m.parts);
                        else
                            ImGui::SetTooltip("%s: %zu models together (e.g. a vehicle and its wheels)\nin %s",
                                              m.name.c_str(), m.pieces.size(), m.group.c_str());
                    }
                    char size[32];
                    snprintf(size, sizeof(size), "%.1f m", m.radius * 2.0f / 100.0f);
                    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(size).x);
                    ImGui::TextDisabled("%s", size);
                    ImGui::PopID();
                }
            }
            i = end;
        }
    }
    ImGui::EndChild();
    ImGui::Text("%d models of other levels in this level", int(CurrentLevel().imports.size()));
    if (!g_otherMessage.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s", g_otherMessage.c_str());
    }
    bool pending = ImportsPending();
    ImGui::BeginDisabled(!pending);
    if (ImGui::Button(pending ? "Load the map again to get them" : "All of them are loaded", ImVec2(-1, 0))) {
        g_otherOpen = false;
        ReloadMapForImports();
    }
    ImGui::EndDisabled();
    if (pending && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Your level stays as it is (save it to keep it on disk).");
    ImGui::End();
}

void DrawPanel()
{
    if (CurrentScene().empty()) {
        ImGui::TextDisabled("no scene loaded");
        return;
    }
    if (g_builtFor != CurrentScene() || g_models.empty())
        Build();
    if (ImGui::Button("Other levels...")) {
        g_otherOpen = true;
        g_otherMessage.clear();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Models of the game's other levels for this level");
    ImGui::SameLine();
    if (ImGui::Button("Refresh"))
        Build();
    ImGui::SameLine();
    ImGui::TextDisabled("%d models in %s", int(g_models.size()), g_builtFor.c_str());
    ImGui::SameLine();
    ImGui::Checkbox("helpers", &g_showHelpers);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "filter by name, class or category", g_filter, sizeof(g_filter));
    if (Placing()) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "placing: move the mouse over the world, click");
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel (Esc)"))
            CancelPlacement();
    } else {
        ImGui::TextDisabled("click a model, then click in the world; right click: more");
    }

    std::string filter = Lower(g_filter);
    auto matches = [&](const ModelEntry& e) {
        return filter.empty() || Lower(e.name).find(filter) != std::string::npos ||
               (!e.level.empty() && Lower(e.level).find(filter) != std::string::npos) ||
               Lower(e.className).find(filter) != std::string::npos ||
               Lower(CategoryName(e.category)).find(filter) != std::string::npos;
    };

    float footer = g_message.empty() ? 0.0f : ImGui::GetTextLineHeightWithSpacing();
    if (ImGui::BeginChild("list", ImVec2(0, -footer), ImGuiChildFlags_Borders)) {
        // categories -> families (only when a family has several models) -> models
        size_t i = 0;
        while (i < g_models.size()) {
            Category cat = g_models[i].category;
            size_t catEnd = i;
            int shown = 0;
            while (catEnd < g_models.size() && g_models[catEnd].category == cat)
                shown += matches(g_models[catEnd++]);
            bool skip = shown == 0 || ((cat == Category::Helpers || cat == Category::Interface) && !g_showHelpers);
            if (!skip) {
                if (!filter.empty())
                    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                char header[96];
                snprintf(header, sizeof(header), "%s  (%d)###cat%d", CategoryName(cat), shown, int(cat));
                if (ImGui::CollapsingHeader(header)) {
                    size_t f = i;
                    while (f < catEnd) {
                        size_t famEnd = f;
                        int famShown = 0;
                        while (famEnd < catEnd && g_models[famEnd].family == g_models[f].family)
                            famShown += matches(g_models[famEnd++]);
                        if (famShown == 1) {
                            for (size_t k = f; k < famEnd; ++k)
                                if (matches(g_models[k]))
                                    DrawRow(g_models[k], int(k));
                        } else if (famShown > 1) {
                            if (!filter.empty())
                                ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                            char label[160];
                            snprintf(label, sizeof(label), "%s  (%d)###fam%d", g_models[f].family.c_str(), famShown,
                                     int(f));
                            if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_SpanAvailWidth)) {
                                for (size_t k = f; k < famEnd; ++k)
                                    if (matches(g_models[k]))
                                        DrawRow(g_models[k], int(k));
                                ImGui::TreePop();
                            }
                        }
                        f = famEnd;
                    }
                }
            }
            i = catEnd;
        }
    }
    ImGui::EndChild();
    if (!g_message.empty())
        ImGui::TextDisabled("%s", g_message.c_str());
    if (g_resort) {
        g_resort = false;
        for (ModelEntry& e : g_models)
            ApplyOverride(e);
        Sort();
    }
}

} // namespace

void InstallModelsPanel()
{
    overlay::AddPanel({"Models", &g_visible, DrawPanel, overlay::Dock::Left});
    static bool always = true;
    overlay::AddPanel({nullptr, &always, ThumbTick});
    overlay::AddPanel({nullptr, &always, DrawOtherLevels});
    OnScenePushed([](const std::string&) {
        g_models.clear();
        g_builtFor.clear();
        g_thumb.parts.clear();  // destroyed with the editor's objects when the scene was left
        g_thumb.offsets.clear();
        g_thumbPath.clear();
        g_viewRoom = nullptr;
        SetThumbnailGeoms({});
    });
}

} // namespace h2::editor
