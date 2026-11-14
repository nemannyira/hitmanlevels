#include "runs.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>

#include "hitmanlevels_version.h"
#include "json.hpp"
#include "levelfmt/crypto.h"
#include "levelfmt/share.h"
#include "../config.h"
#include "../engine/game_events.h"
#include "../engine/messages.h"
#include "../engine/scene_data.h"
#include "../game.h"
#include "../hook.h"
#include "../log.h"
#include "../mainloop.h"
#include "../overlay/overlay.h"
#include "../scene.h"
#include "../script/events.h"
#include "level.h"
#include "mission.h"
#include "model.h"
#include "stages.h"
#include "testtools.h"

namespace h2::editor {

namespace {

using json = nlohmann::json;

// the statistics block as the statistics window keeps it (its handler copies MissionCompleted's here)
constexpr uintptr_t kStatsBlock = 0x6B3418;
// ZWINDOW_StatisticsLevel's message handler (thiscall (msg, param)): MSG_SHOWSTATS fills the screen
constexpr uintptr_t kStatsWindowMessage = 0x55C590;
constexpr size_t kWindow_ShowStatsId = 0x1C6;  // its MSG_SHOWSTATS id (short)
constexpr size_t kWindow_Stealth = 0xB8;       // 0..100 (the upper bar; 100: never seen)
constexpr size_t kWindow_Aggression = 0xBC;    // 0..100 (the lower bar)
constexpr size_t kWindow_Ratings = 0xC6;       // 8 x 8 indexes into the names [stealth band + aggression band * 8]
constexpr uintptr_t kRatingNames = 0x691238;   // Amateur ... MassMurderer (18) ... SilentAssassin (29) ... Wimp
constexpr int kRatingCount = 39;
// the counters while the mission is played (as MissionCompleted 0x5058B0 reads them)
constexpr size_t kLevel_Headshots = 0x208, kLevel_EnemiesHarmed = 0x20C, kLevel_EnemiesKilled = 0x210,
                 kLevel_InnocentsHarmed = 0x214, kLevel_InnocentsKilled = 0x218, kLevel_Alerts = 0x21C,
                 kLevel_CloseEncounters = 0x220, kLevel_Saves = 0x249;
constexpr size_t kHitman_Shots = 0x11C7, kHitman_CloseEncounters = 0x11CB;
constexpr size_t kSys_GameClock = 0x38;  // ms; the statistics' time is it >> 10
constexpr size_t kMaxEvents = 500;
constexpr double kSpottedEveryMs = 10000.0;  // one character's "spotted" kept once in this

bool Read32(const void* base, size_t offset, uint32_t& v)
{
    if (!base)
        return false;
    __try {
        v = *reinterpret_cast<const uint32_t*>(static_cast<const char*>(base) + offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int GameSeconds()
{
    uint32_t ms = 0;
    Read32(game::SysInterface(), kSys_GameClock, ms);
    return int32_t(ms) >> 10;
}

// the statistics screen's lines, in its order
struct Counters {
    uint32_t shots = 0, closeEncounters = 0, headshots = 0, alerts = 0, enemiesKilled = 0, enemiesHarmed = 0,
             innocentsKilled = 0, innocentsHarmed = 0, saves = 0;
    int time = 0;
};

// as MissionCompleted puts them in the block (a run that ends otherwise)
Counters ReadCounters()
{
    Counters c;
    void* level = LevelRoutine();
    void* hitman = engine::Player();
    uint32_t hitmanClose = 0;
    Read32(hitman, kHitman_Shots, c.shots);
    Read32(hitman, kHitman_CloseEncounters, hitmanClose);
    Read32(level, kLevel_CloseEncounters, c.closeEncounters);
    c.closeEncounters += hitmanClose;
    Read32(level, kLevel_Headshots, c.headshots);
    Read32(level, kLevel_Alerts, c.alerts);
    Read32(level, kLevel_EnemiesKilled, c.enemiesKilled);
    Read32(level, kLevel_EnemiesHarmed, c.enemiesHarmed);
    Read32(level, kLevel_InnocentsKilled, c.innocentsKilled);
    Read32(level, kLevel_InnocentsHarmed, c.innocentsHarmed);
    Read32(level, kLevel_Saves, c.saves);
    c.time = GameSeconds();
    return c;
}

// the block (+0x08 .. +0x24 the counters, +0x30 the time, +0x34 the saves)
Counters BlockCounters()
{
    const void* b = reinterpret_cast<const void*>(kStatsBlock);
    Counters c;
    uint32_t time = 0;
    Read32(b, 0x08, c.shots);
    Read32(b, 0x0C, c.closeEncounters);
    Read32(b, 0x10, c.headshots);
    Read32(b, 0x14, c.alerts);
    Read32(b, 0x18, c.enemiesKilled);
    Read32(b, 0x1C, c.enemiesHarmed);
    Read32(b, 0x20, c.innocentsKilled);
    Read32(b, 0x24, c.innocentsHarmed);
    Read32(b, 0x30, time);
    Read32(b, 0x34, c.saves);
    c.time = int(time);
    return c;
}

// the rating as StatisticsLevel_FillLines (0x55BED0) picks it: both scores banded, the window's table
int Band(uint32_t score, uint32_t lowest)
{
    static const uint32_t kUpTo[] = {0x19, 0x26, 0x32, 0x40, 0x4C, 0x55};
    if (score < lowest)
        return 0;
    for (int i = 0; i < 6; ++i)
        if (score < kUpTo[i])
            return i + 1;
    return 7;
}

struct Rating {
    std::string name;
    uint32_t stealth = 0, aggression = 0;
};

bool ReadRating(const void* window, Rating& r)
{
    uint32_t index = 0, name = 0;
    if (!Read32(window, kWindow_Stealth, r.stealth) || !Read32(window, kWindow_Aggression, r.aggression))
        return false;
    int cell = Band(r.stealth, 2) + Band(r.aggression, 3) * 8;
    if (!Read32(window, kWindow_Ratings + size_t(cell) * 4, index) || index >= uint32_t(kRatingCount) ||
        !Read32(reinterpret_cast<const void*>(kRatingNames), index * 4, name) || !name)
        return false;
    char text[64] = {};
    __try {
        strncpy_s(text, reinterpret_cast<const char*>(name), _TRUNCATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    r.name = text;
    return !r.name.empty();
}

struct Run {
    bool active = false;
    std::string id, folder, level, started, file;
    int version = 0;
    json events = json::array();
    int alarmHighest = -1;
    std::map<int, int> goals;  // objective slot -> its state last seen
    bool goalsSeen = false;
    bool dead = false;
    std::map<engine::ZGEOM*, double> spotted;  // who -> when last kept (ms)
};
Run g_run;
bool g_doneThisMap;  // one run a map load
double g_lastPoll;

json HitmanPlace()
{
    engine::ZGEOM* hitman = engine::Player();
    if (!hitman || !IsLive(hitman))
        return nullptr;
    Transform t = GetTransform(hitman);
    return json::array({std::lround(t.pos[0]), std::lround(t.pos[1]), std::lround(t.pos[2])});
}

void AddEvent(json e)
{
    if (g_run.events.size() >= kMaxEvents)
        return;
    e["t"] = GameSeconds();
    g_run.events.push_back(std::move(e));
}

void Start()
{
    const LevelDocument& L = CurrentLevel();
    uint8_t id[16];
    if (!levelfmt::crypto::RandomBytes(id, sizeof(id)))
        return;
    g_run = Run();
    g_run.active = true;
    g_run.id = levelfmt::crypto::Hex(id, sizeof(id));
    g_run.folder = L.folder;
    g_run.level = L.folder.substr(7);  // (Online\<id>)
    g_run.version = L.packageVersion;
    SYSTEMTIME st;
    GetSystemTime(&st);
    char when[32], file[32];
    snprintf(when, sizeof(when), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond);
    snprintf(file, sizeof(file), "%04u%02u%02uT%02u%02u%02uZ.json", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond);
    g_run.started = when;
    g_run.file = file;
    Log("runs: a run of %s (version %d) started", g_run.level.c_str(), g_run.version);
}

// sharing on: levelpack (the game's folder) sends the runs not sent yet, on its own (the game makes no connection)
void StartSender()
{
    std::string exe = GameDir() + "levelpack.exe", cmd = "\"" + exe + "\" stats send --quiet";
    if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("runs: sharing is on, but %s is not there: not sent", exe.c_str());
        return;
    }
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                        nullptr, GameDir().c_str(), &si, &pi)) {
        Log("runs: levelpack not started (%lu)", GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Log("runs: levelpack stats send started");
}

void End(const char* result, const Counters& c, const Rating* rating)
{
    if (!g_run.active)
        return;
    g_run.active = false;
    g_doneThisMap = true;
    json aids = json::array();
    std::string used = AidsUsed();  // ("god mode, game speed")
    for (size_t at = 0; !used.empty() && at != std::string::npos;) {
        size_t comma = used.find(", ", at);
        aids.push_back(used.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
        at = comma == std::string::npos ? comma : comma + 2;
    }
    json dev = json::array();  // H2Fix.ini's settings a real run is played without
    if (g_config.devCommands)
        dev.push_back("DevCommands");
    if (g_config.luaMemory)
        dev.push_back("LuaMemory");
    json j = {
        {"format", 1},
        {"run", g_run.id},
        {"level", g_run.level},
        {"version", g_run.version},
        {"dll", HL_VERSION},
        {"started", g_run.started},
        {"result", result},
        {"time", c.time},
        {"counters",
         {{"shots", c.shots},
          {"close_encounters", c.closeEncounters},
          {"headshots", c.headshots},
          {"alerts", c.alerts},
          {"enemies_killed", c.enemiesKilled},
          {"enemies_harmed", c.enemiesHarmed},
          {"innocents_killed", c.innocentsKilled},
          {"innocents_harmed", c.innocentsHarmed},
          {"saves", c.saves}}},
        {"aids", aids},
        {"dev", dev},
        {"events", g_run.events},
    };
    if (rating) {
        j["rating"] = rating->name;
        j["stealth"] = rating->stealth;
        j["aggression"] = rating->aggression;
    }
    if (IsEscalation())
        j["stage"] = CurrentStage();
    if (g_run.alarmHighest >= 0)
        j["alarm_highest"] = g_run.alarmHighest;
    std::string dir = GameDir() + "Mods\\" + g_run.folder + "\\runs";
    CreateDirectoryA(dir.c_str(), nullptr);
    std::string path = dir + "\\" + g_run.file, tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << j.dump();
    }
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        Log("runs: %s could not be written (%lu)", path.c_str(), GetLastError());
        DeleteFileA(tmp.c_str());
        return;
    }
    Log("runs: %s %s, %d s (%s)", g_run.level.c_str(), result, c.time, g_run.file.c_str());
    if (levelfmt::StatsSharing())
        StartSender();
}

using StatsMessage_t = uint32_t(__fastcall*)(void* self, void* edx, uint32_t msg, char* param);
StatsMessage_t oStatsMessage;

uint32_t __fastcall hkStatsMessage(void* self, void* edx, uint32_t msg, char* param)
{
    uint32_t showId = 0;
    bool show = g_run.active && param && Read32(self, kWindow_ShowStatsId, showId) &&
                uint16_t(msg) == uint16_t(showId);
    uint32_t r = oStatsMessage(self, edx, msg, param);
    if (show) {  // (the block, the scores and the rating are the window's now)
        Rating rating;
        bool rated = ReadRating(self, rating);
        End("completed", BlockCounters(), rated ? &rating : nullptr);
    }
    return r;
}

void OnEvent(const char* event, const engine::GameEvent& e)
{
    if (!g_run.active)
        return;
    std::string ev = event;
    if (ev == "spotted") {
        double now = NowMs();
        auto it = g_run.spotted.find(e.to);
        if (it != g_run.spotted.end() && now - it->second < kSpottedEveryMs)
            return;
        g_run.spotted[e.to] = now;
        AddEvent({{"kind", "spotted"}, {"at", HitmanPlace()}});
    } else if (ev == "mission" && e.type != 1) {  // (not the base level's own end the editor stopped)
        if (e.kind == engine::GameEvent::MissionFailed)
            End("failed", ReadCounters(), nullptr);
        else  // (no statistics screen came)
            End("completed", ReadCounters(), nullptr);
    }
}

void Poll()
{
    if (!g_run.active) {
        const LevelDocument& L = CurrentLevel();
        if (!g_doneThisMap && IsOnlineLevel(L.folder) && LevelIsFor(CurrentScene()) && !LevelApplyPending() &&
            engine::SceneDataValid() && engine::Player() && !overlay::IsOpen())
            Start();
        return;
    }
    if (overlay::IsOpen()) {  // (the editor opened: what follows is not a play of the level)
        End("left", ReadCounters(), nullptr);
        return;
    }
    double now = NowMs();
    if (now - g_lastPoll < 250.0)
        return;
    g_lastPoll = now;
    for (const GoalStatus& g : GoalStatuses()) {
        auto it = g_run.goals.find(g.slot);
        int before = it == g_run.goals.end() ? 0 : it->second;
        g_run.goals[g.slot] = g.state;
        if (g.state == before || (!g_run.goalsSeen && g.state == 1))
            continue;  // (done from the start: a "never spotted" one)
        if (g.state == 1 || g.state == -1)
            AddEvent({{"kind", g.state == 1 ? "objective_done" : "objective_lost"}, {"slot", g.slot}});
    }
    g_run.goalsSeen = true;
    engine::ZGEOM* hitman = engine::Player();
    bool dead = hitman && !engine::IsAlive(hitman);
    if (dead && !g_run.dead)
        AddEvent({{"kind", "died"}, {"at", HitmanPlace()}});
    g_run.dead = dead;
    g_run.alarmHighest = std::max(g_run.alarmHighest, engine::LevelStress());
}

} // namespace

void InstallRuns()
{
    Hook("StatisticsLevel::OnStatsMessage", kStatsWindowMessage, hkStatsMessage, &oStatsMessage);
    script::SetEditorEvents({"spotted", "mission"}, OnEvent);
    OnFrameEnd(Poll);
    OnSceneLeaving([](const std::string&) {
        End("left", ReadCounters(), nullptr);  // (its objects are still there)
    });
    OnScenePushed([](const std::string&) { g_doneThisMap = false; });
}

} // namespace h2::editor
