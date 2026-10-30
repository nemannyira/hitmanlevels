#include "devcmd.h"

#include <windows.h>
#include <cstdio>
#include <fstream>
#include <intrin.h>
#include <algorithm>
#include <map>
#include <new>
#include <tuple>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "config.h"
#include "editor/highlight.h"
#include "editor/history.h"
#include "editor/launcher.h"
#include "editor/level.h"
#include "editor/mission.h"
#include "editor/model.h"
#include "editor/npcs.h"
#include "editor/own_map.h"
#include "editor/playtest.h"
#include "editor/scene_panel.h"
#include "editor/stages.h"
#include "editor/vehicles.h"
#include "engine/collision.h"
#include "engine/game_events.h"
#include "engine/imports.h"
#include "engine/ingame_map.h"
#include "engine/inject.h"
#include "engine/messages.h"
#include "engine/npc.h"
#include "engine/scene_data.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "d3d8types.h"
#include "hook.h"
#include "fov.h"
#include "game.h"
#include "log.h"
#include "overlay/overlay.h"
#include "scene.h"
#include "screenshot.h"
#include "script/lua_vm.h"

namespace h2 {

namespace {

struct Held {
    int code;      // DIK code, or 0x100 + mouse button
    double until;  // NowMs
};
std::mutex g_mutex;
std::vector<Held> g_held;
long g_moveX, g_moveY, g_wheel;  // pending mouse motion and wheel
std::string g_shotPath;

// editor (ImGui) mouse events, one step per frame: move, press, release
struct UiStep {
    int kind;  // 0 move, 1 button down, 2 button up, 3 wheel, 4 key down, 5 key up, 6 char
    float x, y;
    int button;
};
std::vector<UiStep> g_ui;

struct PressRequest {
    std::string label;       // as shown (an id after "##" not part of it)
    int nth = 1;             // the n-th control of that label in the frame
    bool active = false;
    int seen = 0, frames = 0;
    bool found = false;
    float x = 0, y = 0;
    std::string status = "none";
};
PressRequest g_press;
std::map<ImGuiID, ImRect> g_pressRects;  // the frame's items so far

void SetItemHooks(bool on)
{
    if (ImGuiContext* ctx = ImGui::GetCurrentContext())
        ctx->TestEngineHookItems = on;
}

double g_nextPoll;

std::string ModsPath(const char* name) { return GameDir() + "Mods\\" + name; }

void Prune(double now)
{
    for (size_t i = 0; i < g_held.size();)
        if (g_held[i].until <= now)
            g_held.erase(g_held.begin() + i);
        else
            ++i;
}

void RunCommandFile()
{
    std::string path = ModsPath("_cmd.lua");
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return;
    std::stringstream ss;
    ss << f.rdbuf();
    f.close();
    DeleteFileA(path.c_str());
    std::string code = ss.str();
    lua_State* L = script::State();
    std::string out;
    int base = lua_gettop(L);
    if (luaL_loadbuffer(L, code.c_str(), code.size(), "@_cmd.lua") != LUA_OK ||
        script::PCall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
        out = std::string("error: ") + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
    } else {
        for (int i = base + 1; i <= lua_gettop(L); ++i) {
            size_t n;
            const char* s = luaL_tolstring(L, i, &n);
            out += std::string(s, n) + "\n";
            lua_pop(L, 1);
        }
        if (out.empty())
            out = "ok\n";
    }
    lua_settop(L, base);
    Log("devcmd: %s", out.c_str());
    std::ofstream o(ModsPath("_cmd.out"), std::ios::binary | std::ios::trunc);
    o << out;
}

void Poll()
{
    double now = NowMs();
    if (now < g_nextPoll)
        return;
    g_nextPoll = now + 200.0;
    if (script::Untrusted() && !g_config.devCommandsOnline)
        return;  // (a downloaded level runs: not the user's Mods\_cmd.lua either)
    RunCommandFile();
}

// h2.dev_savegame(slot) / h2.dev_loadgame(slot): ZEngineDataBase::SaveGame
// 0x41AAD0 / LoadGame 0x41AF70 (Save\<slot>.1 .. .3; testing the saved state)
bool CallSlot(uintptr_t fn, unsigned slot)
{
    auto sys = reinterpret_cast<char*>(game::SysInterface());
    void* engine = sys ? *reinterpret_cast<void**>(sys + 0x98) : nullptr;
    if (!engine)
        return false;
    __try {
        return reinterpret_cast<bool(__thiscall*)(void*, unsigned)>(fn)(engine, slot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int SaveGameCmd(lua_State* L)
{
    lua_pushboolean(L, CallSlot(0x41AAD0, unsigned(luaL_checkinteger(L, 1))));
    return 1;
}

// a scene or saved game asked for and not come in yet (dev_ready)
bool g_sceneAsked;
long g_framesSinceScene;

int LoadGameCmd(lua_State* L)
{
    bool asked = g_sceneAsked;
    g_sceneAsked = true;  // (before: the scene may come in while the call is under way)
    bool ok = CallSlot(0x41AF70, unsigned(luaL_checkinteger(L, 1)));
    if (!ok)
        g_sceneAsked = asked;
    lua_pushboolean(L, ok);
    return 1;
}

// h2.dev_stage([n]) -> the stage, the count of stages: with n, the level's map loads again for stage n
// (an escalation: editor/stages.h; as the stage bar does; false and why when it does not)
int StageCmd(lua_State* L)
{
    if (lua_gettop(L) >= 1) {
        editor::SetCurrentStage(int(luaL_checkinteger(L, 1)));
        bool asked = g_sceneAsked;
        g_sceneAsked = true;  // (before: the scene may come in while the call is under way)
        std::string err;
        if (!editor::ReloadLevelMap(&err)) {
            g_sceneAsked = asked;
            lua_pushboolean(L, false);
            lua_pushstring(L, err.c_str());
            return 2;
        }
    }
    lua_pushinteger(L, editor::CurrentStage());
    lua_pushinteger(L, editor::StageCount());
    return 2;
}

// h2.dev_roomlists() -> free, all: the scene's room lists of moving objects (engine::RoomListsFree)
int RoomListsCmd(lua_State* L)
{
    lua_pushinteger(L, engine::RoomListsFree());
    lua_pushinteger(L, engine::RoomListCapacity());
    return 2;
}

// h2.dev_export() -> the package's path, or nil and why: Level panel > Share > Export package
int ExportCmd(lua_State* L)
{
    std::string path, error;
    if (!editor::ExportPackage(&path, &error)) {
        lua_pushnil(L);
        lua_pushstring(L, error.c_str());
        return 2;
    }
    lua_pushstring(L, path.c_str());
    return 1;
}

// h2.dev_ready() -> "<scene>|loading" (asked for with dev_loadscene / dev_loadgame, not come in yet),
// "|applying" (the editor puts the level on it: two frames after), "|ready": what tests wait for
int Ready(lua_State* L)
{
    const char* state = g_sceneAsked ? "loading"
                        : editor::LevelApplyPending() || g_framesSinceScene < 3 ? "applying"
                                                                               : "ready";
    lua_pushstring(L, (CurrentScene() + "|" + state).c_str());
    return 1;
}

// h2.dev_edit(op, ...): the editor's own edits (with history), for tests
//   "move", path, x, y, z      a recorded move (as the inspector)
//   "hide", path, on           a recorded hide / show
//   "dup", path, x, y, z       duplicate -> path
//   "delete", path             delete an object the level made
//   "item", template, x, y, z  place an item -> path
//   "point", x, y, z           place a point -> path
//   "rename", path, name       rename a point of the level -> path
//   "removenpc", path          remove a placed character
//   "sky", level               the sky of another level ("" its own)
int EditCmd(lua_State* L)
{
    std::string op = luaL_checkstring(L, 1);
    std::string err;
    auto resolve = [&](int idx) {
        engine::ZGEOM* g = editor::Resolve(luaL_checkstring(L, idx));
        if (!g || !editor::IsLive(g))
            luaL_error(L, "no such object");
        return g;
    };
    auto at = [&](int idx) {
        editor::Transform t{};
        float m[9] = {0, 0, 1, 0, 1, 0, 1, 0, 0};
        memcpy(t.mat, m, sizeof(m));
        for (int i = 0; i < 3; ++i)
            t.pos[i] = float(luaL_checknumber(L, idx + i));
        return t;
    };
    if (op == "move" || op == "hide") {
        engine::ZGEOM* g = resolve(2);
        editor::ObjectState before = editor::GetState(g), after = before;
        if (op == "move")
            for (int i = 0; i < 3; ++i)
                after.transform.pos[i] = float(luaL_checknumber(L, 3 + i));
        else
            after.hidden = lua_toboolean(L, 3) != 0;
        editor::Change(g, before, after, op.c_str());
        return 0;
    }
    engine::ZGEOM* made = nullptr;
    if (op == "dup") {
        editor::Transform t = at(3);
        engine::ZGEOM* g = resolve(2);
        memcpy(t.mat, editor::GetTransform(g).mat, sizeof(t.mat));
        engine::ZGEOM* parent = lua_isstring(L, 6) ? editor::Resolve(lua_tostring(L, 6)) : nullptr;  // (testing)
        made = editor::DuplicateObject(g, &err, &t, parent);
    } else if (op == "delete") {
        lua_pushboolean(L, editor::DeleteObject(resolve(2)));
        return 1;
    } else if (op == "item") {
        engine::ZGEOM* tmpl = resolve(2);
        made = editor::PlaceItem(tmpl, at(3), &err);
    } else if (op == "point") {
        editor::Transform t = at(2);
        made = editor::PlacePoint(t, editor::RoomAt(t.pos), &err);
    } else if (op == "rename") {  // the Inspector's point name
        made = editor::RenamePoint(resolve(2), luaL_checkstring(L, 3), &err);
    } else if (op == "removenpc") {
        lua_pushboolean(L, editor::RemoveNpc(editor::ActorOf(resolve(2)), &err));
        return 1;
    } else if (op == "sky") {
        editor::SetSky(luaL_checkstring(L, 2));
        return 0;
    } else if (op == "npcref") {  // (character, setting, object): the NPCs panel's setting of a reference (kept)
        engine::ZGEOM* actor = editor::ActorOf(resolve(2));
        std::string param = luaL_checkstring(L, 3);
        engine::ScriptInfo s;
        if (!actor || !editor::CurrentBehaviour(actor, s))
            return luaL_error(L, "npcref: no character with a behaviour");
        bool found = false;
        for (engine::ScriptParam& p : s.params)
            if (p.name == param && p.type == engine::ParamType::Ref) {
                p.bits = engine::RefOf(resolve(4));
                found = true;
            }
        if (!found)
            return luaL_error(L, "npcref: no reference %s", param.c_str());
        made = editor::EditNpcBehaviour(actor, &s, &err);
    } else {
        return luaL_error(L, "unknown op %s", op.c_str());
    }
    if (!made)
        return luaL_error(L, "%s: %s", op.c_str(), err.c_str());
    lua_pushstring(L, editor::PathOf(made).c_str());
    return 1;
}

// h2.dev_levelend(completed): the base level's own mission end (as its scripts do)
int LevelEnd(lua_State* L)
{
    lua_pushboolean(L, editor::BaseLevelEnd(lua_toboolean(L, 1) != 0));
    return 1;
}

// h2.dev_loadscene(name): asks the engine to switch scenes (as the menus do)
int LoadScene(lua_State* L)
{
    bool asked = g_sceneAsked;
    g_sceneAsked = true;  // (before: the scene may come in while the call is under way)
    bool ok = RequestScene(luaL_checkstring(L, 1));
    if (!ok)
        g_sceneAsked = asked;
    lua_pushboolean(L, ok);
    return 1;
}

// h2.dev_key(dik, seconds): holds a DirectInput key (DIK_ code)
int Key(lua_State* L)
{
    int dik = int(luaL_checkinteger(L, 1)) & 0xFF;
    double secs = luaL_optnumber(L, 2, 0.1);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_held.push_back({dik, NowMs() + secs * 1000.0});
    return 0;
}

// h2.dev_mouse(button, seconds): holds a mouse button (0 left, 1 right, 2 middle)
int Mouse(lua_State* L)
{
    int button = int(luaL_checkinteger(L, 1)) & 3;
    double secs = luaL_optnumber(L, 2, 0.1);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_held.push_back({0x100 + button, NowMs() + secs * 1000.0});
    return 0;
}

// h2.dev_look(dx, dy): mouse motion (mickeys) the game reads next
int Look(lua_State* L)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_moveX += long(luaL_checkinteger(L, 1));
    g_moveY += long(luaL_optinteger(L, 2, 0));
    return 0;
}

// h2.dev_wheel(notches): mouse wheel (+ up / - down), 120 per notch
int Wheel(lua_State* L)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_wheel += long(luaL_checkinteger(L, 1)) * 120;
    return 0;
}

// h2.dev_tracecams([frames]): logs every camera the renderer draws
int TraceCams(lua_State* L)
{
    TraceCameras(int(luaL_optinteger(L, 1, 1)));
    return 0;
}

// h2.dev_viewdistance(scale)
int ViewDist(lua_State* L)
{
    SetViewDistance(float(luaL_checknumber(L, 1)));
    return 0;
}

// h2.dev_click(x, y [, button [, double]]): moves the editor's mouse to screen
// pixel (x, y) and clicks (0 left, 1 right) - drives the editor UI and its
// clicks in the world
int Click(lua_State* L)
{
    float x = float(luaL_checknumber(L, 1)), y = float(luaL_checknumber(L, 2));
    int button = int(luaL_optinteger(L, 3, 0));
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ui.push_back({0, x, y, button});
    g_ui.push_back({0, x, y, button});
    g_ui.push_back({1, x, y, button});
    g_ui.push_back({2, x, y, button});
    if (lua_toboolean(L, 4)) {
        g_ui.push_back({1, x, y, button});
        g_ui.push_back({2, x, y, button});
    }
    return 0;
}

// h2.dev_hover(x, y): moves the editor's mouse
int Hover(lua_State* L)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ui.push_back({0, float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)), 0});
    return 0;
}

// h2.dev_collide(object, x1, y1, z1, x2, y2, z2 [, mask]) -> x, y, z where the segment meets the
// object's collision (engine/collision.h), or nothing
int Collide(lua_State* L)
{
    engine::ZGEOM* g = editor::Resolve(luaL_checkstring(L, 1));
    float a[3], b[3], hit[3];
    for (int i = 0; i < 3; ++i) {
        a[i] = float(luaL_checknumber(L, 2 + i));
        b[i] = float(luaL_checknumber(L, 5 + i));
    }
    if (!g || !engine::CollisionHit(g, a, b, uint32_t(luaL_optinteger(L, 8, engine::kCollideWalk)), hit))
        return 0;
    for (float v : hit)
        lua_pushnumber(L, v);
    return 3;
}

// h2.dev_uiwheel(notches): the editor's mouse wheel (+ up / - down) where its mouse is (a list, a popup)
int UiWheel(lua_State* L)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ui.push_back({3, 0, float(luaL_checknumber(L, 1)), 0});
    return 0;
}

// h2.dev_type(text): types into the focused editor field
int Type(lua_State* L)
{
    std::string text = luaL_checkstring(L, 1);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (unsigned char c : text)
        g_ui.push_back({6, 0, 0, c});
    return 0;
}

// h2.dev_press(label [, n]): presses the editor's control (button, checkbox, tab, menu item) of
// that label (the n-th of it in a frame; the editor open, its panel shown; panels scrolled to
// it when it is out of sight) in the next frames;
// h2.dev_press_status() -> "looking", "pressed x y" or "not found"
int Press(lua_State* L)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_press = PressRequest{};
    g_press.label = luaL_checkstring(L, 1);
    g_press.nth = int(luaL_optinteger(L, 2, 1));
    g_press.active = true;
    g_press.status = "looking";
    g_pressRects.clear();
    SetItemHooks(true);
    return 0;
}

int PressStatus(lua_State* L)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    lua_pushstring(L, g_press.status.c_str());
    return 1;
}

// h2.dev_uikey(imguiKey): presses an ImGui key in the editor (e.g. 525 = Enter)
int UiKey(lua_State* L)
{
    int key = int(luaL_checkinteger(L, 1));
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ui.push_back({4, 0, 0, key});
    g_ui.push_back({5, 0, 0, key});
    return 0;
}

// h2.dev_shot(name): saves Screens\<name>.bmp at the next frame
int Shot(lua_State* L)
{
    std::string dir = GameDir() + "Screens";
    CreateDirectoryA(dir.c_str(), nullptr);
    g_shotPath = dir + "\\" + luaL_checkstring(L, 1) + ".bmp";
    return 0;
}

// h2.dev_panel(name): shows an editor panel (left tab / right section)
// h2.dev_level(op [, arg]): "new" name | "save" | "saveas" name | "load" folder | "apply" | "revert" | "undo" | "redo" |
// "info" | ...
int Level(lua_State* L)
{
    std::string op = luaL_checkstring(L, 1);
    std::string arg = luaL_optstring(L, 2, "");
    std::string err;
    editor::LevelDocument& d = editor::CurrentLevel();
    if (op == "new") {
        editor::NewLevel(arg.empty() ? "Test" : arg, CurrentScene());
    } else if (op == "newblank") {  // the launcher's New level: emptied, no buildings, the C3-3 yard to stand on
        editor::NewLevel(arg.empty() ? "Test" : arg, CurrentScene());
        editor::SetEmptyMap(true);
        editor::SetBlankMap(true);
        editor::SetFloor("C3-3", 0x60C472, "Yard_06-7-8");
        if (!editor::SaveLevel(&err))
            return luaL_error(L, "save: %s", err.c_str());
    } else if (op == "save") {
        if (!editor::SaveLevel(&err))
            return luaL_error(L, "save: %s", err.c_str());
    } else if (op == "saveas") {  // the toolbar's Save as (a downloaded level: a copy of the user's own)
        if (!editor::SaveLevelAs(arg, &err))
            return luaL_error(L, "saveas: %s", err.c_str());
    } else if (op == "load") {
        if (!editor::LoadLevel(arg, &err))
            return luaL_error(L, "load: %s", err.c_str());
    } else if (op == "apply") {
        lua_pushinteger(L, editor::ApplyLevel());
        return 1;
    } else if (op == "revert") {
        editor::RevertLevel();
    } else if (op == "undo") {
        editor::Undo();
    } else if (op == "redo") {
        editor::Redo();
    } else if (op == "place") {  // "<slot>|<path>": a "bring it" line's place
        size_t bar = arg.find('|');
        int slot = atoi(arg.substr(0, bar).c_str());
        lua_pushstring(L, editor::MissionPlaceFor(slot, bar == std::string::npos ? nullptr
                                                                                  : editor::Resolve(arg.substr(bar + 1)))
                              .c_str());
        return 1;
    } else if (op == "vehicle") {  // "<object>|<point>|<point>...|speed=<cm/s>|loop": a vehicle of the level
        editor::LevelVehicle v;
        size_t start = 0;
        for (int i = 0;; ++i) {
            size_t bar = arg.find('|', start);
            std::string part = arg.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
            if (i == 0)
                v.object = part;
            else if (part.rfind("speed=", 0) == 0)
                v.speed = float(atof(part.c_str() + 6));
            else if (part == "loop")
                v.loop = true;
            else if (part == "reverse")
                v.reverse = true;
            else if (!part.empty())
                v.route.push_back(part);
            if (bar == std::string::npos)
                break;
            start = bar + 1;
        }
        d.vehicles.push_back(v);
        d.dirty = true;
        editor::ApplyVehicles(d.vehicles);
    } else if (op == "ownmap") {  // "<name>": the level's map copied as a map of its own; "original": back
        bool ok = arg == "original" ? editor::BackToOriginalMap(true, &err) : editor::MakeOwnMap(arg, &err);
        if (!ok)
            return luaL_error(L, "ownmap: %s", err.c_str());
        lua_pushstring(L, d.baseScene.c_str());
        return 1;
    } else if (op == "ownlist") {  // "<list path>|<name>": a list of the level's own, a copy (made with the map)
        size_t bar = arg.find('|');
        std::string path = editor::MakeOwnList(editor::Resolve(arg.substr(0, bar)),
                                               bar == std::string::npos ? "" : arg.substr(bar + 1), &err);
        if (path.empty())
            return luaL_error(L, "ownlist: %s", err.c_str());
        lua_pushstring(L, path.c_str());
        return 1;
    } else if (op == "newroute") {  // "<name>": the NPCs panel's New route (a list of the level's own, empty, now)
        engine::ZGEOM* list = editor::MakeNewRoute(arg, &err);
        if (!list)
            return luaL_error(L, "newroute: %s", err.c_str());
        lua_pushstring(L, editor::PathOf(list).c_str());
        return 1;
    } else if (op == "listpoints") {  // "<list path>|<point path>|...": the editor's change of a list (kept)
        std::vector<engine::ZGEOM*> pts;
        size_t start = arg.find('|');
        engine::ZGEOM* list = editor::Resolve(arg.substr(0, start));
        while (start != std::string::npos) {
            size_t bar = arg.find('|', start + 1);
            std::string p = arg.substr(start + 1, bar == std::string::npos ? std::string::npos : bar - start - 1);
            if (engine::ZGEOM* g = editor::Resolve(p))
                pts.push_back(g);
            start = bar;
        }
        if (!editor::SetListPoints(list, pts, &err))
            return luaL_error(L, "listpoints: %s", err.c_str());
    } else if (op == "ownroute") {  // "<character path>|<list setting>": the NPC panel's A route of its own
        size_t bar = arg.find('|');
        std::string path = editor::GiveOwnRoute(editor::ActorOf(editor::Resolve(arg.substr(0, bar))),
                                                bar == std::string::npos ? "" : arg.substr(bar + 1), &err);
        if (path.empty())
            return luaL_error(L, "ownroute: %s", err.c_str());
        lua_pushstring(L, path.c_str());
        return 1;
    } else if (op == "unseen") {  // "fail": being spotted fails the mission
        lua_pushstring(L, editor::MissionUnseen(arg == "fail").c_str());
        return 1;
    } else if (op == "target" || op == "reach" || op == "take" || op == "knockout" || op == "deliver" ||
               op == "exit") {  // the Inspector's Mission row
        lua_pushstring(L, editor::MissionGoalFor(editor::Resolve(arg), op).c_str());
        return 1;
    } else if (op == "mapmodels") {  // "on" / "off": the Level panel's The level's models on the map
        editor::SetMapModels(arg == "on");
    } else if (op == "mapicon") {  // "<point path>|<icon>": the Inspector's On the map ("" none)
        size_t bar = arg.find('|');
        if (!editor::SetPointMapIcon(editor::Resolve(arg.substr(0, bar)),
                                     bar == std::string::npos ? "" : arg.substr(bar + 1), &err))
            return luaL_error(L, "mapicon: %s", err.c_str());
    } else if (op == "blank") {  // the Level panel's No buildings, now ("on" / "off")
        editor::SetBlankMap(arg == "on");
    } else if (op == "solid") {  // "<mesh hex> on|off": a model's collision (reverse engineering)
        uint32_t mesh = uint32_t(strtoul(arg.c_str(), nullptr, 16));
        lua_pushboolean(L, engine::SetModelSolid(mesh, arg.find(" on") != std::string::npos));
        return 1;
    } else if (op == "problems") {
        std::string all;
        for (const std::string& p : editor::LevelProblems())
            all += p + "\n";
        lua_pushstring(L, all.c_str());
        return 1;
    } else if (op != "info") {
        return luaL_error(L, "unknown op %s", op.c_str());
    }
    char buf[512];
    snprintf(buf, sizeof(buf), "%s folder=%s scene=%s objects=%d spawned=%d npcs=%d npc_edits=%d lists=%d floor=%s%s",
             d.name.c_str(), d.folder.c_str(), d.baseScene.c_str(), int(d.objects.size()), int(d.spawned.size()),
             int(d.npcs.size()), int(d.npcEdits.size()), int(d.lists.size()),
             d.floorObject.empty() ? "-" : d.floorObject.c_str(), d.dirty ? " (unsaved)" : "");
    lua_pushstring(L, buf);
    return 1;
}

// h2.dev_select(path | nil): the editor's selection
int SelectCmd(lua_State* L)
{
    engine::ZGEOM* g = lua_isnoneornil(L, 1) ? nullptr : editor::Resolve(luaL_checkstring(L, 1));
    if (!lua_isnoneornil(L, 1) && !g)
        return luaL_error(L, "no such object");
    editor::Select(g);
    return 0;
}

// h2.dev_selected() -> the editor's selection's path (nil: none)
int SelectedCmd(lua_State* L)
{
    engine::ZGEOM* g = editor::Selected();
    if (!g || !editor::IsLive(g))
        return 0;
    lua_pushstring(L, editor::PathOf(g).c_str());
    return 1;
}

// h2.dev_complete(path, partial, method) -> the console's Tab choices, joined by spaces
int CompleteCmd(lua_State* L)
{
    std::string all;
    for (const std::string& n : script::Complete(luaL_checkstring(L, 1), luaL_checkstring(L, 2), lua_toboolean(L, 3)))
        all += (all.empty() ? "" : " ") + n;
    lua_pushstring(L, all.c_str());
    return 1;
}

int Panel(lua_State* L)
{
    overlay::ShowPanel(luaL_checkstring(L, 1));
    return 0;
}

// h2.dev_place_npc(record, x, y, z [, behaviour]): places a character as the
// NPCs panel does (shown until the map loads again) -> its object name
int PlaceNpcCmd(lua_State* L)
{
    editor::NpcSpec spec;
    spec.record = int(luaL_checkinteger(L, 1));
    float m[9] = {0, 0, 1, 0, 1, 0, 1, 0, 0};
    memcpy(spec.transform.mat, m, sizeof(m));
    for (int i = 0; i < 3; ++i)
        spec.transform.pos[i] = float(luaL_checknumber(L, 2 + i));
    if (lua_isstring(L, 5)) {
        if (!editor::BehaviourTemplate(lua_tostring(L, 5), spec.behaviour))
            return luaL_error(L, "no behaviour %s", lua_tostring(L, 5));
        spec.hasBehaviour = true;
    }
    std::string err;
    engine::ZGEOM* g = editor::PlaceNpc(spec, &err);
    if (!g)
        return luaL_error(L, "place: %s", err.c_str());
    lua_pushstring(L, engine::SafeString(g->name).c_str());
    return 1;
}

// h2.dev_setscript(path, script, decl): starts the script on the character
// (decl as a script record has it: "LONG a=1;ZGEOMREF b;"; values: the
// defaults written there, 0 else) -> true / error text
int SetScriptCmd(lua_State* L)
{
    engine::ZGEOM* g = editor::Resolve(luaL_checkstring(L, 1));
    if (!g)
        return luaL_error(L, "no such object");
    engine::ScriptInfo s;
    s.name = luaL_checkstring(L, 2);
    std::string decl = luaL_checkstring(L, 3);
    for (size_t start = 0; start < decl.size();) {
        size_t semi = decl.find(';', start);
        if (semi == std::string::npos)
            semi = decl.size();
        std::string item = decl.substr(start, semi - start), value;
        start = semi + 1;
        if (size_t eq = item.find('='); eq != std::string::npos) {
            value = item.substr(eq + 1);
            item.resize(eq);
        }
        size_t sp = item.rfind(' ');
        if (sp == std::string::npos)
            continue;
        engine::ScriptParam p;
        p.typeName = item.substr(0, sp);
        p.name = item.substr(sp + 1);
        if (p.typeName == "LONG") {
            p.type = engine::ParamType::Long;
            p.bits = uint32_t(atoi(value.c_str()));
        } else if (p.typeName == "FLOAT") {
            p.type = engine::ParamType::Float;
            float f = float(atof(value.c_str()));
            memcpy(&p.bits, &f, 4);
        } else {
            p.type = engine::ParamType::Ref;
        }
        s.params.push_back(p);
    }
    std::vector<uint8_t> block = engine::BuildScriptBlock(s);
    std::string err;
    if (!engine::SetScript(g, &block, &err)) {
        lua_pushstring(L, err.c_str());
        return 1;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// h2.dev_test(): the toolbar's Test
int TestCmd(lua_State*)
{
    editor::TestLevel();
    return 0;
}

// h2.dev_useaction(path [, distance]): a "use" action on the object (engine/messages.h
// AddUseAction, message "H2Editor_Take"); h2.dev_used(path): whether Hitman used it since
int DevUseAction(lua_State* L)
{
    engine::ZGEOM* g = editor::Resolve(luaL_checkstring(L, 1));
    static uint16_t msg;
    if (!msg && (msg = engine::RegisterMessage("H2Editor_Take")) != 0)
        engine::WatchUse(msg);
    lua_pushboolean(L, g && msg && engine::AddUseAction(g, "Take", msg, int(luaL_optinteger(L, 2, 200))));
    return 1;
}

int DevUsed(lua_State* L)
{
    engine::ZGEOM* g = editor::Resolve(luaL_checkstring(L, 1));
    lua_pushboolean(L, g && engine::TakeUsed(g));
    return 1;
}

// h2.dev_copyposdir(target, from): the levels' scripts' teleport (Scene_If CopyObjectPosDir
// 0x57FCB0): the target's world transform set to the other's (geom vfunc 0x84 -> 0x80)
bool SafeCopyPosDir(engine::ZGEOM* target, engine::ZGEOM* from)
{
    __try {
        float mat[9], pos[3];
        (*reinterpret_cast<void(__thiscall***)(engine::ZGEOM*, float*, float*)>(from))[0x84 / 4](from, mat, pos);
        (*reinterpret_cast<void(__thiscall***)(engine::ZGEOM*, float*, float*)>(target))[0x80 / 4](target, mat, pos);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int DevCopyPosDir(lua_State* L)
{
    engine::ZGEOM* t = editor::Resolve(luaL_checkstring(L, 1));
    engine::ZGEOM* f = editor::Resolve(luaL_checkstring(L, 2));
    lua_pushboolean(L, t && f && SafeCopyPosDir(t, f));
    return 1;
}

// h2.dev_setting(path, name) -> a behaviour setting of a character as its running script has it
// (a reference as the object's path)
int DevSetting(lua_State* L)
{
    engine::ZGEOM* g = editor::Resolve(luaL_checkstring(L, 1));
    std::string name = luaL_checkstring(L, 2);
    engine::ScriptInfo s;
    if (!g || !editor::CurrentBehaviour(g, s))
        return 0;
    for (const engine::ScriptParam& p : s.params)
        if (p.name == name) {
            if (p.type == engine::ParamType::Ref) {
                engine::ZGEOM* t = p.bits ? engine::GeomOfRef(p.bits) : nullptr;
                lua_pushstring(L, t ? editor::PathOf(t).c_str() : "");
            } else {
                lua_pushinteger(L, lua_Integer(p.bits));
            }
            return 1;
        }
    return 0;
}

// h2.dev_recordpaths() -> how many of the scene file's records the path inject.cpp builds for
// them (a settings' reference while the scene loads) names otherwise than the editor, and the first ones
int DevRecordPaths(lua_State* L)
{
    const auto& recs = engine::SceneRecords();
    std::vector<std::string> paths(recs.size());
    std::map<std::pair<int, std::string>, int> seen;
    int checked = 0, differ = 0;
    std::string first;
    for (size_t i = 0; i < recs.size() && engine::IsSceneFileRecord(int(i)); ++i) {
        int k = seen[{recs[i].parent, recs[i].name}]++;
        std::string base = recs[i].parent >= 0 && size_t(recs[i].parent) < i ? paths[recs[i].parent] : "ROOT";
        paths[i] = base + "/" + recs[i].name + (k ? "#" + std::to_string(k) : "");
        engine::ZGEOM* g = engine::RecordGeom(int(i));
        if (!g || !editor::IsLive(g))
            continue;
        ++checked;
        std::string real = editor::PathOf(g);
        if (real != paths[i]) {
            ++differ;
            if (real.find("ZWINDOWS") == std::string::npos && real.find("GuiSetup") == std::string::npos)
                first += paths[i] + " | " + real + "\n";  // (the interface's windows are put elsewhere at init)
        }
    }
    lua_pushinteger(L, checked);
    lua_pushinteger(L, differ);
    lua_pushstring(L, first.c_str());
    return 3;
}

// h2.dev_mapicons() -> what the in-game map draws, after it was opened with M (engine/ingame_map.h):
// "floor <name>", then a line per icon: the legend's model and its object's path; 2nd value: the
// window's path
int DevMapIcons(lua_State* L)
{
    if (!engine::IngameMap()) {
        lua_pushstring(L, "none");
        return 1;
    }
    engine::ZGEOM* window = engine::IngameMapWindow();
    std::string out = "floor " + engine::MapShownFloor() + "\n";
    for (const engine::MapIconDrawn& d : engine::MapIconsDrawn())
        out += (d.icon ? engine::SafeString(d.icon->name) : std::string("?")) + " " +
               (d.object ? editor::PathOf(d.object) : std::string("?")) + "\n";
    lua_pushstring(L, out.c_str());
    lua_pushstring(L, window ? editor::PathOf(window).c_str() : "no window");
    return 2;
}

// h2.dev_mapicon(path) -> the object's icon on the in-game map, its own or its group's (a character's):
// "<icon> shown" / "<icon> off" ("none": it has no map routine); 2nd value: the floor the map shows
// the object on (its group's name; "": none)
int DevMapIcon(lua_State* L)
{
    engine::ZGEOM* geom = editor::Resolve(luaL_checkstring(L, 1));
    engine::ZGEOM* owner = engine::MapIconOwner(geom);
    std::string out = owner ? engine::MapIconName(owner) + (engine::MapIconShown(owner) == 1 ? " shown" : " off") : "none";
    engine::ZGEOM* floor = geom ? engine::MapFloorOf(geom) : nullptr;
    lua_pushstring(L, out.c_str());
    lua_pushstring(L, floor ? engine::SafeString(floor->name).c_str() : "");
    return 2;
}

// h2.dev_record(path) -> its record in the scene loaded last (-1: made while playing), "file" /
// "copy" (a copy of the level made with the map) / "added" (the level's characters, lists, ...)
int DevRecord(lua_State* L)
{
    engine::ZGEOM* g = editor::Resolve(luaL_checkstring(L, 1));
    int r = g ? engine::RecordOf(g) : -1;
    lua_pushinteger(L, r);
    lua_pushstring(L, r < 0 ? "none" : engine::IsSceneFileRecord(r) ? "file" : engine::IsSceneCopyRecord(r) ? "copy" : "added");
    return 2;
}

// h2.dev_instantiate(path, x, y, z): a copy made as the game makes one while playing (geom
// vfunc 0x1A8, as ZGEOM_GeomSpawn does: the clone 0x1A4, its init, its routines started) in the
// source's parent, at a world position; returns its path (reverse engineering)
engine::ZGEOM* SafeInstantiate(engine::ZGEOM* source, engine::ZGEOM* parent, const float* mat, const float* pos)
{
    __try {
        auto fn = (*reinterpret_cast<engine::ZGEOM*(__thiscall***)(engine::ZGEOM*, engine::ZGEOM*, const float*,
                                                                    const float*, int, int, int, int)>(source))[0x1A8 / 4];
        return fn(source, parent, mat, pos, 0, 0, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

int DevInstantiate(lua_State* L)
{
    engine::ZGEOM* g = editor::Resolve(luaL_checkstring(L, 1));
    float pos[3] = {float(luaL_checknumber(L, 2)), float(luaL_checknumber(L, 3)), float(luaL_checknumber(L, 4))};
    if (!g || !g->node || !g->node->parent)
        return luaL_error(L, "no such object");
    editor::Transform t = editor::GetTransform(g);
    engine::ZGEOM* made = SafeInstantiate(g, g->node->parent->geom, t.mat, pos);
    if (!made)
        return luaL_error(L, "not made");
    std::string path = editor::PathOf(made);  // (PathOf looks into a fresher snapshot for a new one)
    lua_pushstring(L, path.c_str());
    return 1;
}

// h2.dev_routine(path or a goal zone's name, routine): a routine's fields (reverse engineering)
int DevRoutine(lua_State* L)
{
    std::string n = luaL_checkstring(L, 1);
    engine::ZGEOM* g = editor::Resolve(n);
    if (!g && engine::SceneZoneRecord(n) >= 0)
        g = engine::RecordGeom(engine::SceneZoneRecord(n));
    uint8_t* r = g ? engine::GeomRoutine(g, luaL_checkstring(L, 2)) : nullptr;
    if (!r || !engine::IsReadable(r, 0x90)) {
        lua_pushstring(L, "none");
        return 1;
    }
    char buf[400];
    auto u32 = [&](int o) { uint32_t v; memcpy(&v, r + o, 4); return v; };
    snprintf(buf, sizeof(buf),
             "geom %p routine %p vt %08X flags+8 %08X int+14 %08X t+18 %08X data+38 %08X owner+6E %08X "
             "+72 %08X +76 %08X +7A %04X +7C %04X +7E %02X %02X %02X %02X +82 %08X +88 %02X +89 %02X",
             static_cast<void*>(g), static_cast<void*>(r), u32(0), u32(8), u32(0x14), u32(0x18), u32(0x38), u32(0x6E),
             u32(0x72), u32(0x76), u32(0x7A) & 0xFFFF, u32(0x7C) & 0xFFFF, r[0x7E], r[0x7F], r[0x80], r[0x81], u32(0x82),
             r[0x88], r[0x89]);
    lua_pushstring(L, buf);
    return 1;
}

// h2.dev_editor(open): opens / closes the editor overlay
int Editor(lua_State* L)
{
    overlay::SetOpen(lua_toboolean(L, 1));
    return 0;
}

} // namespace

// ---- sound without focus (automated testing: the game keeps playing in the background) ----

struct DsBufferDesc {
    DWORD dwSize;
    DWORD dwFlags;
};
using CreateSoundBuffer_t = HRESULT(__stdcall*)(void* ds, DsBufferDesc* desc, void** buffer, void* outer);
CreateSoundBuffer_t oCreateSoundBuffer;
HRESULT __stdcall hkCreateSoundBuffer(void* ds, DsBufferDesc* desc, void** buffer, void* outer)
{
    constexpr DWORD kPrimary = 0x1, kGlobalFocus = 0x8000;
    if (desc && !(desc->dwFlags & kPrimary))
        desc->dwFlags |= kGlobalFocus;
    return oCreateSoundBuffer(ds, desc, buffer, outer);
}

void HookSoundObject(void* ds)
{
    if (!ds || oCreateSoundBuffer)
        return;
    void* fn = (*reinterpret_cast<void***>(ds))[3];  // IDirectSound::CreateSoundBuffer
    Hook("IDirectSound::CreateSoundBuffer", fn, hkCreateSoundBuffer, &oCreateSoundBuffer);
}

using DirectSoundCreate_t = HRESULT(WINAPI*)(const GUID*, void**, void*);
DirectSoundCreate_t oDirectSoundCreate, oDirectSoundCreate8;
HRESULT WINAPI hkDirectSoundCreate(const GUID* g, void** ds, void* outer)
{
    HRESULT hr = oDirectSoundCreate(g, ds, outer);
    if (SUCCEEDED(hr) && ds)
        HookSoundObject(*ds);
    return hr;
}
HRESULT WINAPI hkDirectSoundCreate8(const GUID* g, void** ds, void* outer)
{
    HRESULT hr = oDirectSoundCreate8(g, ds, outer);
    if (SUCCEEDED(hr) && ds)
        HookSoundObject(*ds);
    return hr;
}

void InstallBackgroundSound()
{
    if (!LoadLibraryA("dsound.dll"))
        return;
    HookApi("dsound.dll", "DirectSoundCreate", hkDirectSoundCreate, &oDirectSoundCreate);
    HookApi("dsound.dll", "DirectSoundCreate8", hkDirectSoundCreate8, &oDirectSoundCreate8);
}

// ---- callers of a function per object (reverse engineering) ----
// h2.dev_callers(addr, argc): records the return addresses of thiscall calls
// of the function (argc stack arguments, 0-3) per `this`, until
// h2.dev_callers_dump(obj [, clear]) reads them ("ret count" lines).
std::mutex g_callersMutex;
std::map<std::pair<uintptr_t, uintptr_t>, int> g_callers;  // (this, return address) -> calls
void* g_callersOrig[4];

template <int N>
uint32_t __fastcall CallersHook(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c)
{
    {
        std::lock_guard<std::mutex> lock(g_callersMutex);
        ++g_callers[{reinterpret_cast<uintptr_t>(self), reinterpret_cast<uintptr_t>(_ReturnAddress())}];
    }
    if (N == 0)
        return reinterpret_cast<uint32_t(__fastcall*)(void*, void*)>(g_callersOrig[N])(self, edx);
    if (N == 1)
        return reinterpret_cast<uint32_t(__fastcall*)(void*, void*, uint32_t)>(g_callersOrig[N])(self, edx, a);
    if (N == 2)
        return reinterpret_cast<uint32_t(__fastcall*)(void*, void*, uint32_t, uint32_t)>(g_callersOrig[N])(self, edx,
                                                                                                         a, b);
    return reinterpret_cast<uint32_t(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t)>(g_callersOrig[N])(
        self, edx, a, b, c);
}
// fastcall stub per argc so the callee cleans the right number of bytes
uint32_t __fastcall Callers0(void* s, void* e) { return CallersHook<0>(s, e, 0, 0, 0); }
uint32_t __fastcall Callers1(void* s, void* e, uint32_t a) { return CallersHook<1>(s, e, a, 0, 0); }
uint32_t __fastcall Callers2(void* s, void* e, uint32_t a, uint32_t b) { return CallersHook<2>(s, e, a, b, 0); }
uint32_t __fastcall Callers3(void* s, void* e, uint32_t a, uint32_t b, uint32_t c) { return CallersHook<3>(s, e, a, b, c); }

int Callers(lua_State* L)
{
    uintptr_t addr = uintptr_t(luaL_checkinteger(L, 1));
    int argc = int(luaL_optinteger(L, 2, 0));
    void* detour = argc == 0 ? (void*)&Callers0 : argc == 1 ? (void*)&Callers1 : argc == 2 ? (void*)&Callers2 : (void*)&Callers3;
    MH_STATUS st = MH_CreateHook(reinterpret_cast<void*>(addr), detour, &g_callersOrig[argc > 3 ? 3 : argc]);
    if (st == MH_OK)
        st = MH_EnableHook(reinterpret_cast<void*>(addr));
    lua_pushstring(L, MH_StatusToString(st));
    return 1;
}

int CallersDump(lua_State* L)
{
    uintptr_t obj = uintptr_t(luaL_checkinteger(L, 1));
    bool clear = lua_toboolean(L, 2);
    std::string out;
    std::lock_guard<std::mutex> lock(g_callersMutex);
    for (auto it = g_callers.begin(); it != g_callers.end();) {
        if (it->first.first == obj) {
            char line[48];
            snprintf(line, sizeof(line), "%08X %d\n", unsigned(it->first.second), it->second);
            out += line;
        }
        it = clear ? g_callers.erase(it) : std::next(it);
    }
    lua_pushstring(L, out.c_str());
    return 1;
}

// ---- what collision queries return (reverse engineering) ----
// h2.dev_coll(): hooks the collision manager's node gathering (RenderD3D:
// vfunc 0x38 rooms + dynamic, vfunc 0x7c dynamic tree); h2.dev_coll_seen(node)
// -> how often that node was returned since.
std::map<uintptr_t, int> g_collSeen;
using Gather10 = int(__fastcall*)(void*, void*, void**, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                  uint32_t, uint32_t, uint32_t);
using Gather8 = int(__fastcall*)(void*, void*, void**, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                 uint32_t);
Gather10 oGatherNodes;
Gather8 oGatherDynamic;
void Seen(void** out, int n)
{
    std::lock_guard<std::mutex> lock(g_callersMutex);
    for (int i = 0; i < n && i < 4096; ++i)
        ++g_collSeen[reinterpret_cast<uintptr_t>(out[i])];
}
int __fastcall hkGatherNodes(void* self, void* edx, void** out, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                             uint32_t e, uint32_t f, uint32_t g, uint32_t h, uint32_t i)
{
    int n = oGatherNodes(self, edx, out, a, b, c, d, e, f, g, h, i);
    Seen(out, n);
    {
        std::lock_guard<std::mutex> lock(g_callersMutex);
        ++g_callers[{1, reinterpret_cast<uintptr_t>(_ReturnAddress())}];
    }
    return n;
}
int __fastcall hkGatherDynamic(void* self, void* edx, void** out, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                               uint32_t e, uint32_t f, uint32_t g)
{
    int n = oGatherDynamic(self, edx, out, a, b, c, d, e, f, g);
    Seen(out, n);
    {
        std::lock_guard<std::mutex> lock(g_callersMutex);
        ++g_callers[{2, reinterpret_cast<uintptr_t>(_ReturnAddress())}];
    }
    return n;
}

// h2.dev_drawn(path) -> bool: whether the renderer drew the object in the last frames
// h2.dev_locktrace(on): logs the vertex / index buffer locks (reverse engineering): per buffer,
// caller and flags, how many and the bytes locked; h2.dev_locktrace_dump() -> lines
struct LockKey {
    void* buffer;
    void* caller;
    void* outer;  // the caller's caller (flags 0: through RenderD3D's buffer wrappers)
    DWORD flags;
    bool index;
    bool operator<(const LockKey& o) const
    {
        return std::tie(buffer, caller, outer, flags, index) < std::tie(o.buffer, o.caller, o.outer, o.flags, o.index);
    }
};
struct LockStat {
    void* data = nullptr;  // the memory the last lock gave
    unsigned count = 0;
    UINT lo = 0xFFFFFFFF, hi = 0;
};
std::mutex g_lockMutex;
std::map<LockKey, LockStat> g_locks;
bool g_lockTrace;
using BufLock_t = HRESULT(WINAPI*)(void* self, UINT offset, UINT size, BYTE** data, DWORD flags);
void NoteLock(void* self, void* caller, void* outer, UINT offset, UINT size, DWORD flags, bool index)
{
    std::lock_guard<std::mutex> lock(g_lockMutex);
    LockStat& s = g_locks[{self, caller, flags ? nullptr : outer, flags, index}];
    ++s.count;
    s.lo = std::min(s.lo, offset);
    s.hi = std::max(s.hi, size ? offset + size : 0xFFFFFFFFu);
}
// one detour per distinct Lock (the runtime has buffer classes of their own: managed, default, dynamic)
template <int N, bool Index>
struct LockHook {
    static inline BufLock_t original;
    static HRESULT WINAPI Detour(void* self, UINT offset, UINT size, BYTE** data, DWORD flags)
    {
        void* caller = _ReturnAddress();
        void* outer = static_cast<void**>(_AddressOfReturnAddress())[6];
        HRESULT hr = original(self, offset, size, data, flags);
        if (g_lockTrace) {
            NoteLock(self, caller, outer, offset, size, flags, Index);
            if (hr >= 0 && data) {
                std::lock_guard<std::mutex> lock(g_lockMutex);
                g_locks[{self, caller, flags ? nullptr : outer, flags, Index}].data = *data;
            }
        }
        return hr;
    }
};
std::vector<void*> g_lockTargets;
template <int N, bool Index>
void HookLock(void* buffer)
{
    void* target = d3d8::VtableEntry(buffer, 11);
    if (std::find(g_lockTargets.begin(), g_lockTargets.end(), target) != g_lockTargets.end())
        return;
    g_lockTargets.push_back(target);
    Hook(Index ? "IDirect3DIndexBuffer8::Lock" : "IDirect3DVertexBuffer8::Lock", target, &LockHook<N, Index>::Detour,
         &LockHook<N, Index>::original);
    Log("locktrace: %s Lock %p hooked", Index ? "IB" : "VB", target);
}
bool g_lockHooked;
int LockTrace(lua_State* L)
{
    bool on = lua_toboolean(L, 1) != 0;
    if (on && !g_lockHooked) {
        void* dev = editor::HighlightDevice();
        if (!dev)
            return luaL_error(L, "no device");
        g_lockHooked = true;
        auto release = [](void* b) { reinterpret_cast<ULONG(WINAPI*)(void*)>(d3d8::VtableEntry(b, 2))(b); };
        auto vb = [&](DWORD usage, DWORD pool) {
            void* b = nullptr;
            reinterpret_cast<HRESULT(WINAPI*)(void*, UINT, DWORD, DWORD, DWORD, void**)>(d3d8::VtableEntry(dev, 23))(
                dev, 64, usage, 0x2, pool, &b);
            return b;
        };
        auto ib = [&](DWORD usage, DWORD pool) {
            void* b = nullptr;
            reinterpret_cast<HRESULT(WINAPI*)(void*, UINT, DWORD, DWORD, DWORD, void**)>(d3d8::VtableEntry(dev, 24))(
                dev, 64, usage, 101, pool, &b);
            return b;
        };
        // usage 0x8 write only, 0x200 dynamic; pool 0 default, 1 managed, 2 system memory
        void* v[4] = {vb(0, 1), vb(0x8, 0), vb(0x208, 0), vb(0, 2)};
        void* i[4] = {ib(0, 1), ib(0x8, 0), ib(0x208, 0), ib(0, 2)};
        if (v[0]) HookLock<0, false>(v[0]);
        if (v[1]) HookLock<1, false>(v[1]);
        if (v[2]) HookLock<2, false>(v[2]);
        if (v[3]) HookLock<3, false>(v[3]);
        if (i[0]) HookLock<4, true>(i[0]);
        if (i[1]) HookLock<5, true>(i[1]);
        if (i[2]) HookLock<6, true>(i[2]);
        if (i[3]) HookLock<7, true>(i[3]);
        for (void* b : v)
            if (b)
                release(b);
        for (void* b : i)
            if (b)
                release(b);
    }
    {
        std::lock_guard<std::mutex> lock(g_lockMutex);
        if (on)
            g_locks.clear();
        g_lockTrace = on;
    }
    lua_pushinteger(L, lua_Integer(g_lockTargets.size()));
    return 1;
}
int LockTraceDump(lua_State* L)
{
    uintptr_t render = reinterpret_cast<uintptr_t>(GetModuleHandleA("RenderD3D.dll"));
    std::ostringstream out;
    std::lock_guard<std::mutex> lock(g_lockMutex);
    for (const auto& [k, s] : g_locks) {
        char line[200];
        uintptr_t c = reinterpret_cast<uintptr_t>(k.caller);
        uintptr_t o = reinterpret_cast<uintptr_t>(k.outer);
        snprintf(line, sizeof(line), "%s %p caller %s+%X outer %X flags %lX count %u bytes %X..%X data %p",
                 k.index ? "IB" : "VB", k.buffer, c >= render && c < render + 0x200000 ? "RenderD3D" : "?",
                 unsigned(c >= render ? c - render : c), unsigned(o >= render && o < render + 0x200000 ? o - render : o),
                 k.flags, s.count, s.lo, s.hi, s.data);
        out << line << '\n';
    }
    lua_pushstring(L, out.str().c_str());
    return 1;
}

int Drawn(lua_State* L)
{
    engine::ZGEOM* g = editor::Resolve(luaL_checkstring(L, 1));
    lua_pushboolean(L, g && editor::IsLive(g) && editor::WasDrawnRecently(g->node));
    return 1;
}

int Coll(lua_State* L)
{
    uintptr_t render = reinterpret_cast<uintptr_t>(GetModuleHandleA("RenderD3D.dll"));
    if (!render)
        return luaL_error(L, "no RenderD3D.dll");
    bool ok = Hook("GatherNodes", render + 0x28C80, &hkGatherNodes, &oGatherNodes) &&
              Hook("GatherDynamic", render + 0x28DE0, &hkGatherDynamic, &oGatherDynamic);
    lua_pushboolean(L, ok);
    return 1;
}
int CollSeen(lua_State* L)
{
    uintptr_t node = uintptr_t(luaL_checkinteger(L, 1));
    std::lock_guard<std::mutex> lock(g_callersMutex);
    auto it = g_collSeen.find(node);
    lua_pushinteger(L, it == g_collSeen.end() ? 0 : it->second);
    return 1;
}

int CrashCmd(lua_State* L);  // (below, with the Present hook)

void InstallDevCommands()
{
    if (!g_config.devCommands)
        return;
    InstallBackgroundSound();
    script::Register("dev_loadscene", LoadScene);
    script::Register("dev_savegame", SaveGameCmd);
    script::Register("dev_loadgame", LoadGameCmd);
    script::Register("dev_levelend", LevelEnd);
    script::Register("dev_edit", EditCmd);
    script::Register("dev_key", Key);
    script::Register("dev_mouse", Mouse);
    script::Register("dev_look", Look);
    script::Register("dev_wheel", Wheel);
    script::Register("dev_tracecams", TraceCams);
    script::Register("dev_viewdistance", ViewDist);
    script::Register("dev_shot", Shot);
    script::Register("dev_editor", Editor);
    script::Register("dev_routine", DevRoutine);
    script::Register("dev_useaction", DevUseAction);
    script::Register("dev_used", DevUsed);
    script::Register("dev_record", DevRecord);
    script::Register("dev_mapicons", DevMapIcons);
    script::Register("dev_mapicon", DevMapIcon);
    script::Register("dev_copyposdir", DevCopyPosDir);
    script::Register("dev_setting", DevSetting);
    script::Register("dev_recordpaths", DevRecordPaths);
    script::Register("dev_instantiate", DevInstantiate);
    script::Register("dev_place_npc", PlaceNpcCmd);
    script::Register("dev_test", TestCmd);
    script::Register("dev_setscript", SetScriptCmd);
    script::Register("dev_panel", Panel);
    script::Register("dev_select", SelectCmd);
    script::Register("dev_selected", SelectedCmd);
    script::Register("dev_complete", CompleteCmd);
    script::Register("dev_level", Level);
    script::Register("dev_click", Click);
    script::Register("dev_hover", Hover);
    script::Register("dev_type", Type);
    script::Register("dev_uiwheel", UiWheel);
    script::Register("dev_collide", Collide);
    script::Register("dev_uikey", UiKey);
    script::Register("dev_press", Press);
    script::Register("dev_press_status", PressStatus);
    script::Register("dev_callers", Callers);
    script::Register("dev_callers_dump", CallersDump);
    script::Register("dev_coll", Coll);
    script::Register("dev_drawn", Drawn);
    script::Register("dev_locktrace", LockTrace);
    script::Register("dev_meshtrace", [](lua_State* L) {
        editor::TraceMeshDraws(uint32_t(luaL_optinteger(L, 1, 0)));
        return 0;
    });
    script::Register("dev_meshtrace_dump", [](lua_State* L) {
        lua_pushstring(L, editor::TracedMeshDraws().c_str());
        return 1;
    });
    script::Register("dev_locktrace_dump", LockTraceDump);
    script::Register("dev_coll_seen", CollSeen);
    script::Register("dev_crash", CrashCmd);
    script::Register("dev_ready", Ready);
    script::Register("dev_roomlists", RoomListsCmd);
    script::Register("dev_stage", StageCmd);
    script::Register("dev_launcher", [](lua_State*) {  // the main menu's Level editor window
        editor::OpenLevelLauncher();
        return 0;
    });
    script::Register("dev_export", ExportCmd);
    OnScenePushed([](const std::string&) {
        g_sceneAsked = false;
        g_framesSinceScene = 0;
    });
    Log("dev commands on: Mods\\_cmd.lua");
}

bool DevInjecting()
{
    if (!g_config.devCommands)
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    Prune(NowMs());
    return !g_held.empty() || g_moveX || g_moveY || g_wheel;
}

void DevInjectKeyboard(unsigned char* keys)
{
    if (!g_config.devCommands)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    Prune(NowMs());
    for (const Held& h : g_held)
        if (h.code < 0x100)
            keys[h.code] = 0x80;
}

void DevInjectMouse(long* dx, long* dy, unsigned char* buttons, long* wheel)
{
    if (!g_config.devCommands)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    Prune(NowMs());
    for (const Held& h : g_held)
        if (h.code >= 0x100)
            buttons[h.code - 0x100] = 0x80;
    *dx += g_moveX;
    *dy += g_moveY;
    g_moveX = g_moveY = 0;
    if (wheel) {
        *wheel += g_wheel;
        g_wheel = 0;
    }
}

// the injected mouse position stays a while (the backend adds the OS cursor every frame)
float g_holdX, g_holdY;
double g_holdUntil;

// ---- an editor control pressed by its label (h2.dev_press), through ImGui's test engine hooks:
// while a press is asked for, every item of a frame reports its box (ItemAdd) and its label
// (ItemInfo); the one asked for gets a click in its middle, as h2.dev_click gives one
// the n-th control of the label: its middle (half out of its panel: not yet, scrolled on)
void PressMatch(const ImRect& rect)
{
    if (++g_press.seen != g_press.nth)
        return;
    ImVec2 mid = rect.GetCenter();
    if (!GImGui->CurrentWindow->ClipRect.Contains(mid))
        return;
    g_press.found = true;
    g_press.x = mid.x;
    g_press.y = mid.y;
}

// a control of the label's own id in its window (the id ImGui gives that label there: every control
// added, a combo too, which gives no label to the hooks below)
void PressItemAdd(ImGuiID id, const ImRect& bb)
{
    if (!g_press.active || g_press.found)
        return;
    g_pressRects[id] = bb;
    if (id == GImGui->CurrentWindow->GetID(g_press.label.c_str()))
        PressMatch(bb);
}

// a control whose label shows it ("edit##take": "edit") under another id
void PressItemInfo(ImGuiID id, const char* label)
{
    if (!g_press.active || g_press.found || !label || id == GImGui->CurrentWindow->GetID(g_press.label.c_str()))
        return;  // (counted when it was added)
    std::string shown = label;
    shown = shown.substr(0, shown.find("##"));
    if (shown != g_press.label)
        return;
    if (auto it = g_pressRects.find(id); it != g_pressRects.end())
        PressMatch(it->second);
}

// a control below a panel's visible part is not drawn (ImGui clips it before its label is
// given): the scrollable windows go over their contents, top to bottom, two frames a step
bool PressScroll(int frame)
{
    bool more = false;
    for (ImGuiWindow* w : GImGui->Windows) {
        if (!w->Active || w->ScrollMax.y <= 0)
            continue;
        if (frame == 3) {
            ImGui::SetScrollY(w, 0);
            more = true;
        } else if (w->Scroll.y < w->ScrollMax.y) {
            ImGui::SetScrollY(w, ImMin(w->ScrollMax.y, w->Scroll.y + w->InnerRect.GetHeight() * 0.7f));
            more = true;
        }
    }
    return more;
}

// at the start of the editor's frame: the last frame's search done -> the click, or one more frame
void PressFrame()
{
    if (!g_press.active)
        return;
    if (g_press.found) {
        g_ui.push_back({0, g_press.x, g_press.y, 0});
        g_ui.push_back({0, g_press.x, g_press.y, 0});
        g_ui.push_back({1, g_press.x, g_press.y, 0});
        g_ui.push_back({2, g_press.x, g_press.y, 0});
        char buf[64];
        snprintf(buf, sizeof(buf), "pressed %.0f %.0f", g_press.x, g_press.y);
        g_press.status = buf;
        g_press.active = false;
        SetItemHooks(false);
    } else if (++g_press.frames > 120 ||  // (a panel being shown may take a frame or two)
               (g_press.frames >= 3 && g_press.frames % 2 == 1 && !PressScroll(g_press.frames))) {
        g_press.status = "not found";
        g_press.active = false;
        SetItemHooks(false);
    } else {
        SetItemHooks(true);
    }
    g_press.seen = 0;
    g_pressRects.clear();
}

void DevUiStep()
{
    if (!g_config.devCommands || !ImGui::GetCurrentContext())
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    PressFrame();
    ImGuiIO& io = ImGui::GetIO();
    if (NowMs() < g_holdUntil)
        io.AddMousePosEvent(g_holdX, g_holdY);
    if (g_ui.empty())
        return;
    UiStep s = g_ui.front();
    g_ui.erase(g_ui.begin());
    if (s.kind <= 2) {
        g_holdX = s.x;
        g_holdY = s.y;
        g_holdUntil = NowMs() + 2000.0;
    }
    switch (s.kind) {
    case 0: io.AddMousePosEvent(s.x, s.y); break;
    // the backend adds the OS cursor every frame while the game is in front: clicks bring their position
    case 1: io.AddMousePosEvent(s.x, s.y); io.AddMouseButtonEvent(s.button, true); break;
    case 2: io.AddMousePosEvent(s.x, s.y); io.AddMouseButtonEvent(s.button, false); break;
    case 3: io.AddMouseWheelEvent(0.0f, s.y); break;
    case 4: io.AddKeyEvent(ImGuiKey(s.button), true); break;
    case 5: io.AddKeyEvent(ImGuiKey(s.button), false); break;
    case 6: io.AddInputCharacter(unsigned(s.button)); break;
    }
}

// h2.dev_crash(kind): the crash report tried (crashlog.h) at the next frame, outside Lua: "av" a write
// through null, "throw" a C++ exception nobody catches, "hang" no frame for 35 s
std::string g_crashKind;

int CrashCmd(lua_State* L)
{
    g_crashKind = luaL_checkstring(L, 1);
    return 0;
}

void CrashNow()
{
    std::string kind = g_crashKind;
    g_crashKind.clear();
    Log("devcmd: crash test: %s", kind.c_str());
    if (kind == "av")
        *static_cast<volatile int*>(nullptr) = 1;
    else if (kind == "throw")
        throw std::bad_alloc();
    else if (kind == "hang")
        for (ULONGLONG until = GetTickCount64() + 35000; GetTickCount64() < until;)
            Sleep(10);
}

void DevOnPresent(void* device)
{
    TraceFrameDone();
    if (g_config.devCommands)
        Poll();  // at Present, like the editor UI (outside the game logic)
    ++g_framesSinceScene;
    if (!g_crashKind.empty())
        CrashNow();
    if (g_shotPath.empty())
        return;
    std::string path = g_shotPath;
    g_shotPath.clear();
    if (!SaveBackBuffer(device, path) && !SaveScreenshot(device, path))
        Log("devcmd: screenshot %s failed", path.c_str());
}

} // namespace h2

// ImGui's test engine hooks (CMakeLists: IMGUI_ENABLE_TEST_ENGINE): only h2.dev_press turns them on
void ImGuiTestEngineHook_ItemAdd(ImGuiContext*, ImGuiID id, const ImRect& bb, const ImGuiLastItemData*)
{
    h2::PressItemAdd(id, bb);
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags)
{
    h2::PressItemInfo(id, label);
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }

