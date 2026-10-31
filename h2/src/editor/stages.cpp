#include "stages.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "imgui.h"
#include "json.hpp"

#include "../hook.h"
#include "../log.h"
#include "../scene.h"
#include "../script/events.h"
#include "../script/lua_vm.h"
#include "../script/state.h"
#include "../engine/npc.h"
#include "model.h"
#include "mission.h"
#include "profile_guard.h"

namespace h2::editor {

namespace {

using json = nlohmann::json;

constexpr uintptr_t kLoadGame = 0x41AF70;  // ZEngineDataBase::LoadGame(slot): reads the saved map's name, pushes it

int g_stage = 1;           // the stage the map is loaded / played for
bool g_noted;              // a rule only noted as broken in this play of the stage
bool g_failed;             // the stage was failed by a rule (once)
std::string g_message;     // the Stages section's last message

LevelDocument& L() { return CurrentLevel(); }

// ---- the rules ----

struct RuleKind {
    const char* kind;
    const char* label;  // the editor's list
};
const RuleKind kRuleKinds[] = {
    {"unseen", "Never be seen"},
    {"time", "Time limit"},
    {"no_innocents", "No innocents killed"},
    {"no_disguise", "No disguise (the start clothes)"},
    {"one_disguise", "One disguise at most"},
    {"kill_with", "Targets killed only with..."},
    {"no_firearm", "No firearm drawn"},
    {"no_bodies", "No bodies found"},
};

// "kill_with": the weapons a target may be killed with, by the kind of the weapon in Hitman's hands
// (engine::WeaponKind, the template's "Type"), or one weapon (its template's name)
struct WeaponChoice {
    const char* value;
    const char* label;
};
const WeaponChoice kWeaponChoices[] = {
    {"fiberwire", "the fiber wire"},  {"knife", "a knife or blade"}, {"closecombat", "close combat (no firearm)"},
    {"pistol", "a pistol or revolver"}, {"firearm", "a firearm"},
};

std::string WeaponText(const std::string& weapon)
{
    for (const WeaponChoice& c : kWeaponChoices)
        if (weapon == c.value)
            return c.label;
    return weapon;  // (a weapon's own name: "GunHardballer")
}

const char* RuleLabel(const std::string& kind)
{
    for (const RuleKind& k : kRuleKinds)
        if (kind == k.kind)
            return k.label;
    return "?";
}

// what the laptop and the screen say of a rule
std::string RuleText(const StageRule& r)
{
    if (r.kind == "time") {
        char buf[64];
        snprintf(buf, sizeof(buf), "Within %d:%02d", r.seconds / 60, r.seconds % 60);
        return buf;
    }
    if (r.kind == "kill_with")
        return "Targets only with " + WeaponText(r.weapon);
    return RuleLabel(r.kind);
}

std::vector<int> ActiveRules()
{
    std::vector<int> out;
    for (size_t i = 0; i < L().rules.size(); ++i)
        if (InStage(L().rules[i].span))
            out.push_back(int(i));
    return out;
}

// h2._stagerules() -> { {i =, kind =, seconds =, fails =, text =}, ... } (the stage's)
int LuaStageRules(lua_State* s)
{
    lua_newtable(s);
    int n = 0;
    for (int i : ActiveRules()) {
        const StageRule& r = L().rules[size_t(i)];
        lua_createtable(s, 0, 5);
        lua_pushinteger(s, i + 1);
        lua_setfield(s, -2, "i");
        lua_pushstring(s, r.kind.c_str());
        lua_setfield(s, -2, "kind");
        lua_pushinteger(s, r.seconds);
        lua_setfield(s, -2, "seconds");
        lua_pushboolean(s, r.fails);
        lua_setfield(s, -2, "fails");
        lua_pushstring(s, RuleText(r).c_str());
        lua_setfield(s, -2, "text");
        if (r.kind == "kill_with") {
            lua_pushstring(s, r.weapon.c_str());
            lua_setfield(s, -2, "weapon");
            lua_newtable(s);  // the stage's targets (their objects' paths; none: every kill of Hitman's)
            int t = 0;
            for (const MissionObjective& o : CurrentLevel().mission.objectives)
                if (o.goal == MissionObjective::Kill && InStage(o.span))
                    if (engine::ZGEOM* g = ResolveTarget(o.target)) {
                        lua_pushstring(s, PathOf(g).c_str());
                        lua_rawseti(s, -2, ++t);
                    }
            lua_setfield(s, -2, "targets");
        }
        lua_rawseti(s, -2, ++n);
    }
    return 1;
}

// h2._stagebroken(i, why [, quiet]): a rule was broken (quiet: told again after a saved game loaded)
int LuaStageBroken(lua_State* s)
{
    lua_Integer i = luaL_checkinteger(s, 1) - 1;
    std::string why = luaL_optstring(s, 2, "");
    bool quiet = lua_toboolean(s, 3);
    if (i < 0 || size_t(i) >= L().rules.size())
        return 0;
    const StageRule& r = L().rules[size_t(i)];
    if (!r.fails) {
        g_noted = true;
        if (!quiet)
            ShowInfo(InfoKind::Warning, why + " (the stage does not count as clean)");
        Log("stages: rule %d (%s) broken, noted: %s", int(i) + 1, r.kind.c_str(), why.c_str());
        return 0;
    }
    if (g_failed || quiet)
        return 0;
    g_failed = true;
    Log("stages: rule %d (%s) broken, the stage failed: %s", int(i) + 1, r.kind.c_str(), why.c_str());
    ShowInfo(InfoKind::Warning, "Stage failed: " + why);
    FailMission();
    return 0;
}

// h2.stage() -> the stage played, the count of stages (0 for a plain level), its name
int LuaStage(lua_State* s)
{
    lua_pushinteger(s, IsEscalation() ? CurrentStage() : 0);
    lua_pushinteger(s, StageCount());
    lua_pushstring(s, IsEscalation() ? CurrentLevel().stages[size_t(CurrentStage() - 1)].name.c_str() : "");
    return 3;
}

// The rules, on the game's events (the level's handlers: dropped with them). Their state is in
// h2.state.__stage (saved with saved games): the rules broken, the time played.
const char* kRulesLua = R"lua(
local stage, count, name = h2.stage()
local rules = h2._stagerules()
local fresh = h2.state.__stage == nil  -- (a saved game's comes back with it)
h2.state.__stage = h2.state.__stage or { broken = {}, played = 0 }
if fresh then  -- the stage and its rules on screen as it starts
  local text = "Stage " .. stage .. " of " .. count .. (name ~= "" and (": " .. name) or "")
  local list = {}
  for _, r in ipairs(rules) do list[#list + 1] = r.text end
  if #list > 0 then text = text .. ". Rules: " .. table.concat(list, ", ") end
  h2.after(2, function() h2.subtitle(text, 8) end)
end
if #rules == 0 then return end
local S = h2.state.__stage
S.broken = S.broken or {}
local since = h2.time()
local function played() return (S.played or 0) + h2.time() - since end
local function broke(r, why)
  if S.broken[tostring(r.i)] then return end
  S.broken[tostring(r.i)] = true
  h2._stagebroken(r.i, why)
end
h2.on("save", function() S.played = played(); since = h2.time() end)
h2.on("load", function()
  S = h2.state.__stage or S
  since = h2.time()
  for _, r in ipairs(rules) do
    if S.broken[tostring(r.i)] then h2._stagebroken(r.i, "", true) end
  end
end)
local FIREARM = { pistol = true, revolver = true, smg = true, machinegun = true, rifle = true, pumpgun = true,
                  shotgun = true, rocketlauncher = true }
for _, r in ipairs(rules) do
  if r.kind == "unseen" then
    h2.on("spotted", function() broke(r, "You were seen") end)
  elseif r.kind == "no_innocents" then
    h2.on("kill", function(e) if e.kind == "innocent" then broke(r, "An innocent was killed") end end)
  elseif r.kind == "no_disguise" then
    h2.on("disguise", function() broke(r, "You changed clothes") end)
  elseif r.kind == "one_disguise" then
    h2.on("disguise", function()
      S.disguises = (S.disguises or 0) + 1
      if S.disguises > 1 then broke(r, "A second disguise") end
    end)
  elseif r.kind == "kill_with" then
    -- the weapon in Hitman's hands when a target dies (h2.hand: its kind, its template's name)
    local function check()
      local _, kind, name = h2.hand()
      kind, name = kind or "", (name or ""):lower()
      local ok = (r.weapon == "fiberwire" and kind == "fiberwire") or (r.weapon == "knife" and kind == "knife") or
        (r.weapon == "pistol" and (kind == "pistol" or kind == "revolver")) or
        (r.weapon == "closecombat" and (kind == "knife" or kind == "fiberwire" or kind == "other")) or
        (r.weapon == "firearm" and FIREARM[kind]) or (name ~= "" and name == r.weapon:lower())
      if not ok then broke(r, "A target killed with another weapon") end
    end
    if #r.targets > 0 then  -- the stage's targets (their deaths: the editor's, a character without a behaviour too)
      h2.on("death", function(who)
        local p = who and who:path() or ""
        for _, t in ipairs(r.targets) do if t == p then check() return end end
      end)
    else  -- no target: every kill the game counts as Hitman's
      h2.on("kill", function(e) if e.kind ~= "other" then check() end end)
    end
  elseif r.kind == "no_firearm" then
    h2.every(0.5, function(t)
      local _, kind = h2.hand()
      if kind and FIREARM[kind] then broke(r, "You drew a firearm") t:cancel() end
    end)
  elseif r.kind == "no_bodies" then
    -- the game's own sight (TrackLinkObjects, ZLIST_CheckVisible): a body is one of what the characters look
    -- out for once dead (Msg_ObjectAdd); ObjectVisible goes to one who sees it (#30)
    h2.on("msg:objectvisible", function(e)
      local body, seer, hitman = e.object, e.to, h2.player()
      if not body or not seer or not body:isa("ZActor") or not seer:isa("ZActor") then return end
      if hitman and (body:path() == hitman:path() or seer:path() == hitman:path()) then return end
      if (not body:alive() or body:knockedout()) and seer:alive() and not seer:knockedout() then
        broke(r, "A body was found")
      end
    end)
  elseif r.kind == "time" then
    local told = {}
    local function tick()
      local left = math.floor(r.seconds - played())
      if left <= 0 then broke(r, "Time is up"); return end
      for _, at in ipairs({600, 300, 120, 60, 30, 10}) do
        if left <= at and left > at - 2 and not told[at] then
          told[at] = true
          h2.info(string.format("%d:%02d left", left // 60, left % 60), "hint")
        end
      end
      h2.after(1, tick)
    end
    h2.after(1, tick)
  end
end
)lua";

// ---- progress ----

std::string ProgressPath() { return GameDir() + "Save\\escalations.json"; }

json ReadProgress()
{
    std::ifstream f(ProgressPath(), std::ios::binary | std::ios::ate);
    if (!f || f.tellg() > std::streamoff(1 << 20))
        return json::object();
    f.seekg(0);
    json j = json::parse(f, nullptr, false);
    return j.is_object() ? j : json::object();
}

void WriteProgress(const json& j)
{
    std::string path = ProgressPath(), tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << j.dump(1);
        if (!f) {
            Log("stages: cannot write %s", tmp.c_str());
            return;
        }
    }
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        Log("stages: cannot write %s", path.c_str());
}

// the level's mission was completed: the stage is done
void OnMissionCompleted()
{
    if (!IsEscalation() || L().folder.empty() || CurrentScene() != L().baseScene || g_failed)
        return;
    json j = ReadProgress();
    json& p = j[L().folder];
    if (!p.is_object())
        p = json::object();
    int done = std::max(p.value("done", 0), g_stage);
    p["done"] = done;
    json clean = p.value("clean", json::array());
    if (!g_noted && std::find(clean.begin(), clean.end(), g_stage) == clean.end())
        clean.push_back(g_stage);
    p["clean"] = clean;
    WriteProgress(j);
    Log("stages: %s stage %d of %d done%s", L().folder.c_str(), g_stage, StageCount(), g_noted ? " (not clean)" : "");
    ShowInfo(InfoKind::Info, g_stage >= StageCount() ? "Escalation complete"
                                                     : "Stage " + std::to_string(g_stage + 1) + " opened");
}

// ---- a saved game's stage ----

using LoadGame_t = bool(__fastcall*)(void* engine, void* edx, unsigned slot);
LoadGame_t oLoadGame;

bool __fastcall hkLoadGame(void* engine, void* edx, unsigned slot)
{
    // the stage it was saved in, before the map is pushed (what is made with it is that stage's)
    std::ifstream f(script::StateFile(slot), std::ios::binary | std::ios::ate);
    if (f && f.tellg() < std::streamoff(4 << 20) && IsEscalation()) {
        f.seekg(0);
        json j = json::parse(f, nullptr, false);
        if (j.is_object() && j.contains("__editor") && j["__editor"].is_object()) {
            int stage = StageOfSavedGame(j["__editor"].dump());
            if (stage > 0) {
                Log("stages: saved game %u is of stage %d", slot, stage);
                SetCurrentStage(stage);
            }
        }
    }
    return oLoadGame(engine, edx, slot);
}

void OnScene(const std::string&)
{
    g_noted = false;
    g_failed = false;
}

// ---- editor ----

void Changed() { L().dirty = true; }

// the map loaded for another stage (saved first)
void GoToStage(int stage)
{
    SetCurrentStage(stage);
    std::string err;
    if (!ReloadLevelMap(&err))
        g_message = "not loaded: " + err;
}

// a stage taken away: its things come in the stage before; the later ones' a stage earlier
void RemoveStage(int stage)
{
    auto fix = [&](StageSpan& s) {
        if (s.from >= stage)
            s.from = std::max(s.from - 1, 0);
        if (s.gone > stage)
            --s.gone;
        else if (s.gone == stage)
            s.gone = 0;
        if (s.from <= 1)
            s.from = 0;
    };
    LevelDocument& d = L();
    for (SpawnedObject& s : d.spawned)
        fix(s.span);
    for (LevelNpc& n : d.npcs)
        fix(n.span);
    for (auto& [path, e] : d.objects)
        fix(e.span);
    for (MissionObjective& o : d.mission.objectives)
        fix(o.span);
    for (StageRule& r : d.rules)
        fix(r.span);
    d.stages.erase(d.stages.begin() + (stage - 1));
    Changed();
}

void DrawRule(StageRule& r, int index, int& remove)
{
    ImGui::PushID(index);
    ImGui::BulletText("%s", RuleText(r).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("x"))
        remove = index;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Take the rule away");
    ImGui::Indent();
    if (r.kind == "kill_with") {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        if (ImGui::BeginCombo("with", WeaponText(r.weapon).c_str())) {
            for (const WeaponChoice& c : kWeaponChoices)
                if (ImGui::Selectable(c.label, r.weapon == c.value)) {
                    r.weapon = c.value;
                    Changed();
                }
            ImGui::Separator();  // one weapon of the map (its template)
            for (const engine::GeomInfo& g : RecentSceneModel().geoms)
                if (g.geom && g.className.find("ItemTemplateWeapon") != std::string::npos &&
                    ImGui::Selectable(g.name.c_str(), r.weapon == g.name)) {
                    r.weapon = g.name;
                    Changed();
                }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The weapon in Hitman's hands when a target of the stage dies (no target: any kill\n"
                              "of his): a kind of weapon, or one weapon of the map");
    }
    if (r.kind == "time") {
        int minutes = r.seconds / 60, seconds = r.seconds % 60;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
        bool c = ImGui::InputInt("min", &minutes, 1, 5);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
        c |= ImGui::InputInt("s", &seconds, 10, 30);
        if (c) {
            r.seconds = std::clamp(minutes * 60 + seconds, 10, 4 * 3600);
            Changed();
        }
    }
    int broken = r.fails ? 0 : 1;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (ImGui::Combo("broken", &broken, "fails the stage\0only noted\0")) {
        r.fails = broken == 0;
        Changed();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fails the stage: the mission ends failed, with the reason on screen.\n"
                          "Only noted: said on screen; the stage done does not count as clean.");
    if (DrawStageSpan(r.span, "##rulespan"))
        Changed();
    ImGui::Unindent();
    ImGui::PopID();
}

} // namespace

// ---- the stage ----

bool IsEscalation() { return !CurrentLevel().stages.empty(); }

int StageCount() { return int(CurrentLevel().stages.size()); }

int CurrentStage() { return IsEscalation() ? std::clamp(g_stage, 1, StageCount()) : 1; }

void SetCurrentStage(int stage) { g_stage = std::clamp(stage, 1, kMaxStages); }

bool InStage(const StageSpan& span, int stage)
{
    if (!IsEscalation())
        return true;
    return stage >= std::max(span.from, 1) && (span.gone <= 0 || stage < span.gone);
}

bool InStage(const StageSpan& span) { return InStage(span, CurrentStage()); }

StageSpan NewSpan()
{
    StageSpan s;
    if (IsEscalation() && CurrentStage() > 1)
        s.from = CurrentStage();
    return s;
}

std::string SpanText(const StageSpan& span)
{
    if (!IsEscalation() || (span.from <= 1 && span.gone <= 0))
        return "";
    int from = std::max(span.from, 1);
    if (span.gone <= 0)
        return "from stage " + std::to_string(from);
    if (span.gone == from + 1)
        return "stage " + std::to_string(from) + " only";
    return "stages " + std::to_string(from) + "-" + std::to_string(span.gone - 1);
}

std::string StageBriefing(const std::string& briefing)
{
    if (!IsEscalation())
        return briefing;
    const LevelStage& st = CurrentLevel().stages[size_t(CurrentStage() - 1)];
    std::string out = "Stage " + std::to_string(CurrentStage()) + " of " + std::to_string(StageCount());
    if (!st.name.empty())
        out += ": " + st.name;
    out += "\n";
    if (!st.text.empty())
        out += st.text + "\n";
    std::vector<int> rules = ActiveRules();
    if (!rules.empty()) {
        out += "Rules:\n";
        for (int i : rules)
            out += "- " + RuleText(CurrentLevel().rules[size_t(i)]) + "\n";
    }
    if (!briefing.empty())
        out += "\n" + briefing;
    return out;
}

void RunStageRules()
{
    if (!IsEscalation())
        return;
    if (!script::ExecChunk(kRulesLua, "=stage rules"))
        Log("stages: the rules did not start");
}

void OnLevelLoaded()
{
    if (g_stage > StageCount())
        g_stage = 1;
}

int StageOfSavedGame(const std::string& extra)
{
    json j = json::parse(extra, nullptr, false);
    if (!j.is_object() || j.value("level", "") != CurrentLevel().name)
        return 0;
    int stage = j.value("stage", 0);
    return stage >= 1 && stage <= StageCount() ? stage : 0;
}

int StagesDone(const std::string& folder)
{
    json j = ReadProgress();
    if (!j.contains(folder) || !j[folder].is_object())
        return 0;
    int done = j[folder].value("done", 0);
    return std::clamp(done, 0, kMaxStages);
}

bool StageDoneCleanly(const std::string& folder, int stage)
{
    json j = ReadProgress();
    if (!j.contains(folder) || !j[folder].is_object())
        return false;
    json clean = j[folder].value("clean", json::array());
    return std::find(clean.begin(), clean.end(), stage) != clean.end();
}

bool StagePlayable(const std::string& folder, int stage, int count, bool replay)
{
    int done = StagesDone(folder);
    int next = std::min(done + 1, count);
    return stage == next || (replay && stage >= 1 && stage <= done);
}

// ---- editor ----

void DrawStageBar()
{
    if (!IsEscalation())
        return;
    ImGui::TextDisabled("Stage");
    for (int k = 1; k <= StageCount(); ++k) {
        ImGui::SameLine(0, 2);
        bool current = k == CurrentStage();
        if (current)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        char label[16];
        snprintf(label, sizeof(label), "%d##stage", k);
        if (ImGui::Button(label) && !current)
            GoToStage(k);
        if (current)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            const LevelStage& st = CurrentLevel().stages[size_t(k - 1)];
            ImGui::SetTooltip("Stage %d%s%s\n%s", k, st.name.empty() ? "" : ": ", st.name.c_str(),
                              current ? "(shown: what is placed now comes in this stage)"
                                      : "(saves the level and loads the map for that stage)");
        }
    }
    if (StageCount() < kMaxStages) {
        ImGui::SameLine(0, 2);
        if (ImGui::Button("+##stage")) {
            CurrentLevel().stages.push_back({"Stage " + std::to_string(StageCount() + 1), ""});
            Changed();
            GoToStage(StageCount());
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A new stage after the last (everything of the stages before is in it)");
    }
}

void DrawStagesSection()
{
    LevelDocument& d = CurrentLevel();
    if (!ImGui::CollapsingHeader("Stages (escalation)"))
        return;
    if (!IsEscalation()) {
        ImGui::TextWrapped("An escalation is played in stages (1 to 10), one after another: each has what the "
                           "ones before have, and more (characters, objects, objectives, rules).");
        if (ImGui::Button("Make it an escalation")) {
            d.stages.push_back({"Stage 1", ""});
            SetCurrentStage(1);
            Changed();
        }
        return;
    }
    if (ImGui::Checkbox("Stages done can be played again", &d.replay))
        Changed();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The player may play the stages done again (the next one is always open)");
    ImGui::TextDisabled("Shown and edited: stage %d (the toolbar's Stage buttons)", CurrentStage());
    int removeStage = 0;
    for (int k = 1; k <= StageCount(); ++k) {
        LevelStage& st = d.stages[size_t(k - 1)];
        ImGui::PushID(k);
        char header[160];
        snprintf(header, sizeof(header), "Stage %d: %s###stage", k, st.name.c_str());
        ImGui::SetNextItemOpen(k == CurrentStage(), ImGuiCond_Appearing);
        if (ImGui::TreeNode(header)) {
            char name[64];
            snprintf(name, sizeof(name), "%s", st.name.c_str());
            ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 3);
            if (ImGui::InputText("name", name, sizeof(name)) && st.name != name) {
                st.name = name;
                Changed();
            }
            char text[1024];
            snprintf(text, sizeof(text), "%s", st.text.c_str());
            ImGui::InputTextMultiline("##text", text, sizeof(text), ImVec2(-1, ImGui::GetTextLineHeight() * 3));
            if (ImGui::IsItemDeactivatedAfterEdit() && st.text != text) {
                st.text = text;
                Changed();
            }
            if (ImGui::IsItemHovered() && st.text.empty())
                ImGui::SetTooltip("What the laptop's briefing says of this stage (before its rules)");
            ImGui::TextDisabled("Rules coming in this stage:");
            int remove = -1;
            for (size_t i = 0; i < d.rules.size(); ++i)
                if (std::max(d.rules[i].span.from, 1) == k)
                    DrawRule(d.rules[i], int(i), remove);
            if (remove >= 0) {
                d.rules.erase(d.rules.begin() + remove);
                Changed();
            }
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::BeginCombo("##addrule", "Add a rule...")) {
                for (const RuleKind& rk : kRuleKinds)
                    if (ImGui::Selectable(rk.label)) {
                        StageRule r;
                        r.kind = rk.kind;
                        r.span.from = k > 1 ? k : 0;
                        d.rules.push_back(r);
                        Changed();
                    }
                ImGui::EndCombo();
            }
            if (StageCount() > 1 && ImGui::SmallButton("Take this stage away"))
                removeStage = k;
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (removeStage > 0) {
        RemoveStage(removeStage);
        if (CurrentStage() >= removeStage && g_stage > 1)
            GoToStage(g_stage - 1);
    }
    if (StageCount() == 1 && d.rules.empty() && ImGui::SmallButton("Back to a plain level")) {
        d.stages.clear();
        Changed();
    }
    if (!g_message.empty())
        ImGui::TextWrapped("%s", g_message.c_str());
}

bool DrawStageSpan(StageSpan& span, const char* id)
{
    if (!IsEscalation())
        return false;
    ImGui::PushID(id);
    bool changed = false;
    int count = StageCount(), from = std::max(span.from, 1);
    int to = span.gone > 0 ? span.gone - 1 : count;  // the last stage it is in
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("stages");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3);
    if (ImGui::BeginCombo("##from", std::to_string(from).c_str())) {
        for (int k = 1; k <= count; ++k)
            if (ImGui::Selectable(std::to_string(k).c_str(), k == from)) {
                span.from = k > 1 ? k : 0;
                if (span.gone > 0 && span.gone <= k)
                    span.gone = 0;
                changed = true;
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("to");
    ImGui::SameLine();
    std::string last = span.gone > 0 ? std::to_string(to) : "the last";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
    if (ImGui::BeginCombo("##to", last.c_str())) {
        for (int k = from; k < count; ++k)
            if (ImGui::Selectable(std::to_string(k).c_str(), span.gone == k + 1)) {
                span.gone = k + 1;
                changed = true;
            }
        if (ImGui::Selectable("the last", span.gone <= 0)) {
            span.gone = 0;
            changed = true;
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The stages it is there in (\"the last\": stages added later too).\n"
                          "Changed, it shows so when the map is loaded for a stage again.");
    ImGui::PopID();
    return changed;
}

void InstallStages()
{
    script::Register("_stagerules", LuaStageRules);
    script::Register("_stagebroken", LuaStageBroken);
    script::Register("stage", LuaStage);
    Hook("ZEngineDataBase::LoadGame", kLoadGame, hkLoadGame, &oLoadGame);
    SetMissionCompletedListener(OnMissionCompleted);
    OnScenePushed(OnScene);
}

} // namespace h2::editor
