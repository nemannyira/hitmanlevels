#include "npc_panel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"

#include "../engine/npc.h"
#include "../engine/scene_data.h"
#include "../game.h"
#include "../log.h"
#include "../overlay/overlay.h"
#include "../scene.h"
#include "guides.h"
#include "highlight.h"
#include "model.h"
#include "scene_panel.h"

namespace h2::editor {

namespace {

// a kind of character: one actor record of the scene per model + behaviour
struct NpcType {
    int record = -1;
    std::string model;     // "BodyguardMafia_01"
    std::string group;     // name of the group it is in ("BodyguardMafia_Bedroom_00")
    std::string script;    // short behaviour name ("BaseGuard")
    int count = 0;
};

// a behaviour of the scene: script class + the parameters of its first use
struct Behaviour {
    std::string shortName;
    engine::ScriptInfo info;  // references as runtime refs
    int uses = 0;
};

bool g_visible = true;
std::string g_builtFor;
std::vector<NpcType> g_types;
std::vector<Behaviour> g_behaviours;
int g_type = -1;            // chosen character
int g_behaviour = -1;       // chosen behaviour (-1: none)
engine::ScriptInfo g_edit;  // behaviour being edited for the next NPC
std::vector<engine::ZGEOM*> g_spawned;
std::string g_message;
int g_counter;

// placing
bool g_placing;
bool g_pickPending;
int g_pickFrame;
bool g_haveHit;
float g_point[3];
engine::ZGEOM* g_hitRoom;

engine::ScriptInfo Runtime(engine::ScriptInfo s)
{
    for (engine::ScriptParam& p : s.params)
        if (p.type == engine::ParamType::Ref)
            p.bits = p.bits ? engine::RemapRef(p.bits) : 0;
    return s;
}

void Build()
{
    g_types.clear();
    g_behaviours.clear();
    g_type = g_behaviour = -1;
    g_edit = {};
    g_builtFor = CurrentScene();
    const auto& records = engine::SceneRecords();
    std::map<std::string, int> typeIndex, behaviourIndex;
    for (const engine::SceneRecord& r : records) {
        if (!r.script)
            continue;
        engine::ScriptInfo info;
        if (!engine::ParseScript(r.script, info))
            continue;
        std::string shortName = engine::ScriptShortName(info.name);
        auto [it, added] = behaviourIndex.try_emplace(info.name, int(g_behaviours.size()));
        if (added)
            g_behaviours.push_back({shortName, Runtime(info), 0});
        g_behaviours[it->second].uses++;
    }
    for (int index : engine::ActorRecords()) {
        const engine::SceneRecord& r = records[index];
        NpcType t;
        t.record = index;
        t.model = engine::ActorModelName(r);
        if (t.model.empty())
            t.model = r.name;
        t.group = r.parent >= 0 ? records[r.parent].name : "";
        engine::ScriptInfo info;
        if (r.script && engine::ParseScript(r.script, info))
            t.script = engine::ScriptShortName(info.name);
        std::string key = t.model + "|" + t.script;
        auto [it, added] = typeIndex.try_emplace(key, int(g_types.size()));
        if (added)
            g_types.push_back(t);
        g_types[it->second].count++;
    }
    std::sort(g_types.begin(), g_types.end(), [](const NpcType& a, const NpcType& b) {
        return a.script != b.script ? a.script < b.script : a.model < b.model;
    });
}

void ChooseBehaviour(int b)
{
    g_behaviour = b;
    g_edit = b >= 0 ? g_behaviours[b].info : engine::ScriptInfo{};
}

void ChooseType(int t)
{
    g_type = t;
    const auto& records = engine::SceneRecords();
    const engine::SceneRecord& r = records[g_types[t].record];
    engine::ScriptInfo info;
    int b = -1;
    if (r.script && engine::ParseScript(r.script, info)) {
        for (size_t i = 0; i < g_behaviours.size(); ++i)
            if (g_behaviours[i].info.name == info.name)
                b = int(i);
        g_behaviour = b;
        g_edit = Runtime(info);  // this character's own parameters
    } else {
        ChooseBehaviour(-1);
    }
}

std::string RefLabel(uint32_t ref)
{
    if (!ref)
        return "(none)";
    engine::ZGEOM* g = engine::GeomOfRef(ref);
    if (!g || !IsLive(g))
        return "(missing)";
    return engine::SafeString(g->name);
}

engine::ZGEOM* ActorOf(engine::ZGEOM* g)
{
    engine::ZBaseGeom* node = g && IsLive(g) ? g->node : nullptr;
    for (int depth = 0; node && depth < 64; ++depth) {
        if (!engine::IsReadable(node, sizeof(*node)))
            return nullptr;
        if (node->geom && IsLive(node->geom) && engine::IsA(node->geom, "ZActor"))
            return node->geom;
        node = node->parent;
    }
    return nullptr;
}

// Parameter table; `editable` false shows the values only.
void DrawParams(engine::ScriptInfo& s, bool editable)
{
    if (s.params.empty()) {
        ImGui::TextDisabled("no parameters");
        return;
    }
    if (!ImGui::BeginTable("params", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        return;
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.3f);
    engine::ZGEOM* sel = Selected();
    for (size_t i = 0; i < s.params.size(); ++i) {
        engine::ScriptParam& p = s.params[i];
        ImGui::PushID(int(i));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(p.name.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s %s", p.typeName.c_str(), p.name.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1);
        switch (p.type) {
        case engine::ParamType::Long: {
            int v = int(p.bits);
            if (!editable)
                ImGui::Text("%d", v);
            else if (ImGui::InputInt("##v", &v, 0))
                p.bits = uint32_t(v);
            break;
        }
        case engine::ParamType::Float: {
            float v;
            memcpy(&v, &p.bits, 4);
            if (!editable)
                ImGui::Text("%.2f", v);
            else if (ImGui::InputFloat("##v", &v, 0, 0, "%.2f"))
                memcpy(&p.bits, &v, 4);
            break;
        }
        case engine::ParamType::Ref: {
            std::string label = RefLabel(p.bits);
            if (!editable) {
                ImGui::TextUnformatted(label.c_str());
                break;
            }
            ImGui::TextUnformatted(label.c_str());
            if (p.bits && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", PathOf(engine::GeomOfRef(p.bits)).c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(!sel || !IsLive(sel));
            if (ImGui::SmallButton("= selected"))
                p.bits = engine::RefOf(sel);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Use the selected object (e.g. a waypoint or guard point)");
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                p.bits = 0;
            break;
        }
        case engine::ParamType::String: {
            if (!editable) {
                ImGui::TextUnformatted(p.text.c_str());
                break;
            }
            char buf[256];
            snprintf(buf, sizeof(buf), "%s", p.text.c_str());
            if (ImGui::InputText("##v", buf, sizeof(buf)))
                p.text = buf;
            break;
        }
        default:
            ImGui::TextDisabled("(%s)", p.typeName.c_str());
            break;
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void DrawSelectedNpc()
{
    engine::ZGEOM* actor = ActorOf(Selected());
    if (!actor)
        return;
    ImGui::SeparatorText("Selected character");
    int record = engine::RecordOf(actor);
    bool ours = std::find(g_spawned.begin(), g_spawned.end(), actor) != g_spawned.end();
    if (record < 0) {
        ImGui::TextDisabled(ours ? "placed by the editor" : "not from the scene file");
        return;
    }
    const auto& records = engine::SceneRecords();
    const engine::SceneRecord& r = records[record];
    ImGui::Text("%s", engine::ActorModelName(r).c_str());
    ImGui::TextDisabled("record %d, in %s", record, r.parent >= 0 ? records[r.parent].name.c_str() : "-");
    engine::ScriptInfo info;
    if (!r.script || !engine::ParseScript(r.script, info)) {
        ImGui::TextDisabled("no behaviour");
        return;
    }
    ImGui::Text("Behaviour: %s", engine::ScriptShortName(info.name).c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", info.name.c_str());
    ImGui::TextDisabled("as authored in the level:");
    engine::ScriptInfo shown = Runtime(info);
    DrawParams(shown, false);
    std::string model = engine::ActorModelName(r), script = engine::ScriptShortName(info.name);
    for (size_t t = 0; t < g_types.size(); ++t) {
        if (g_types[t].model != model || g_types[t].script != script)
            continue;
        if (ImGui::SmallButton("Use as the next NPC")) {
            ChooseType(int(t));
            g_edit = shown;  // with this character's own parameters
        }
        break;
    }
}

void DrawPanel()
{
    if (CurrentScene().empty()) {
        ImGui::TextDisabled("no scene loaded");
        return;
    }
    if (engine::SceneRecords().empty()) {
        ImGui::TextWrapped("The scene's data was not captured: load the level again (it is read while loading).");
        return;
    }
    if (g_builtFor != CurrentScene())
        Build();

    ImGui::SeparatorText("Characters");
    ImGui::TextDisabled("%d kinds, %d behaviours in this level", int(g_types.size()), int(g_behaviours.size()));
    float listH = ImGui::GetTextLineHeightWithSpacing() * 8;
    if (ImGui::BeginChild("types", ImVec2(0, listH), ImGuiChildFlags_Borders)) {
        for (size_t i = 0; i < g_types.size(); ++i) {
            const NpcType& t = g_types[i];
            char label[256];
            snprintf(label, sizeof(label), "%s##t%d", t.model.c_str(), int(i));
            if (ImGui::Selectable(label, g_type == int(i), ImGuiSelectableFlags_AllowOverlap))
                ChooseType(int(i));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\nbehaviour: %s\n%d in the level (e.g. %s)", t.model.c_str(),
                                  t.script.empty() ? "none" : t.script.c_str(), t.count, t.group.c_str());
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(t.script.c_str()).x);
            ImGui::TextDisabled("%s", t.script.c_str());
        }
    }
    ImGui::EndChild();

    if (g_type >= 0) {
        ImGui::SeparatorText("Behaviour of the next NPC");
        const char* current = g_behaviour >= 0 ? g_behaviours[g_behaviour].shortName.c_str() : "none";
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##behaviour", current)) {
            if (ImGui::Selectable("none (stands still)", g_behaviour < 0))
                ChooseBehaviour(-1);
            for (size_t i = 0; i < g_behaviours.size(); ++i) {
                char label[200];
                snprintf(label, sizeof(label), "%s  (%d)", g_behaviours[i].shortName.c_str(), g_behaviours[i].uses);
                if (ImGui::Selectable(label, g_behaviour == int(i)))
                    ChooseBehaviour(int(i));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", g_behaviours[i].info.name.c_str());
            }
            ImGui::EndCombo();
        }
        if (g_behaviour >= 0)
            DrawParams(g_edit, true);
        if (g_placing) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "click in the world to place the NPC");
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel (Esc)"))
                CancelNpcPlacement();
        } else if (ImGui::Button("Place NPC", ImVec2(-1, 0))) {
            g_placing = true;
            g_pickPending = false;
            g_haveHit = false;
            g_message.clear();
        }
    } else {
        ImGui::TextDisabled("choose a character to place one");
    }

    DrawSelectedNpc();

    if (!g_spawned.empty()) {
        ImGui::SeparatorText("Placed NPCs (until the level is reloaded)");
        for (size_t i = 0; i < g_spawned.size(); ++i) {
            engine::ZGEOM* g = g_spawned[i];
            ImGui::PushID(int(i));
            bool live = IsLive(g);
            char label[160];
            snprintf(label, sizeof(label), "%s%s", live ? engine::SafeString(g->name).c_str() : "(gone)",
                     live && Selected() == g ? "  (selected)" : "");
            if (ImGui::Selectable(label, live && Selected() == g) && live)
                Select(g);
            ImGui::PopID();
        }
    }
    if (!g_message.empty())
        ImGui::TextWrapped("%s", g_message.c_str());
}

void OnPick(engine::ZGEOM* hit, const float* point, const float*)
{
    g_pickPending = false;
    g_haveHit = point != nullptr;
    if (point)
        memcpy(g_point, point, sizeof(g_point));
    if (hit)
        if (engine::ZGEOM* room = RoomOf(hit))
            g_hitRoom = room;
}

void Place()
{
    if (g_type < 0 || !g_haveHit)
        return;
    engine::NpcSpawn s;
    s.record = g_types[g_type].record;
    s.parent = g_hitRoom && IsLive(g_hitRoom) ? g_hitRoom : nullptr;
    if (!s.parent) {
        g_message = "no room under the mouse";
        return;
    }
    // facing the camera
    game::ZCAMERA* cam = game::MainCamera();
    float f[3] = {0, 0, 1};
    if (cam) {
        float m[9], c[3];
        game::GetWorldTransform(cam, m, c);
        f[0] = c[0] - g_point[0];
        f[2] = c[2] - g_point[2];
        float len = std::sqrt(f[0] * f[0] + f[2] * f[2]);
        if (len > 1.0f) {
            f[0] /= len;
            f[2] /= len;
        } else {
            f[0] = 0;
            f[2] = 1;
        }
    }
    float m[9] = {f[0], 0, f[2], 0, 1, 0, f[2], 0, -f[0]};  // rows forward, up, right
    memcpy(s.mat, m, sizeof(m));
    memcpy(s.pos, g_point, sizeof(g_point));
    if (g_behaviour >= 0)
        s.script = engine::BuildScriptBlock(g_edit);
    char name[96];
    snprintf(name, sizeof(name), "NPC_%s_%d", g_types[g_type].model.c_str(), ++g_counter);
    s.name = name;
    std::string err;
    engine::ZGEOM* npc = engine::SpawnNpc(s, &err);
    if (!npc) {
        g_message = "placing failed: " + err;
        return;
    }
    AdoptEngineOwned(npc);
    g_spawned.push_back(npc);
    Select(npc);
    g_message = std::string("placed ") + name +
                (g_behaviour >= 0 ? " with behaviour " + g_behaviours[g_behaviour].shortName : " without behaviour");
}

} // namespace

bool NpcPlacing() { return g_placing; }

void CancelNpcPlacement()
{
    g_placing = false;
    g_pickPending = false;
}

void UpdateNpcPlacement()
{
    if (!g_placing)
        return;
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || g_type < 0) {
        CancelNpcPlacement();
        return;
    }
    bool overWorld = !io.WantCaptureMouse && !ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (!overWorld)
        return;
    if (g_pickPending && ImGui::GetFrameCount() - g_pickFrame > 10)
        g_pickPending = false;
    if (!g_pickPending) {
        g_pickPending = true;
        g_pickFrame = ImGui::GetFrameCount();
        RequestPick(io.MousePos.x, io.MousePos.y, OnPick);
    }
    if (g_haveHit) {
        DrawSnapTarget(g_point, IM_COL32(64, 224, 255, 255));
        float top[3] = {g_point[0], g_point[1] + 180.0f, g_point[2]};
        DrawLine3D(g_point, top, IM_COL32(64, 224, 255, 200), 2.0f);  // about a person's height
    }
    char text[256];
    snprintf(text, sizeof(text), "%s (%s)\nclick: place   Esc: cancel%s", g_types[g_type].model.c_str(),
             g_behaviour >= 0 ? g_behaviours[g_behaviour].shortName.c_str() : "no behaviour",
             g_haveHit ? "" : "\nno surface under the mouse");
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 p(io.MousePos.x + 18, io.MousePos.y + 18);
    ImVec2 size = ImGui::CalcTextSize(text);
    dl->AddRectFilled(ImVec2(p.x - 6, p.y - 4), ImVec2(p.x + size.x + 6, p.y + size.y + 4), IM_COL32(0, 0, 0, 170), 4);
    dl->AddText(p, IM_COL32(255, 220, 120, 255), text);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && g_haveHit) {
        Place();
        if (!io.KeyShift)
            CancelNpcPlacement();
    }
}

void InstallNpcPanel()
{
    engine::InstallSceneData();
    overlay::AddPanel({"NPCs", &g_visible, DrawPanel, overlay::Dock::Left});
    OnScenePushed([](const std::string&) {
        g_builtFor.clear();
        g_types.clear();
        g_behaviours.clear();
        g_spawned.clear();
        g_type = g_behaviour = -1;
        g_hitRoom = nullptr;
        CancelNpcPlacement();
    });
}

} // namespace h2::editor
