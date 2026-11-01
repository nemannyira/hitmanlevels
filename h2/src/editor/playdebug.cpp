#include "playdebug.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "imgui.h"

#include "../config.h"
#include "../engine/game_events.h"
#include "../engine/messages.h"
#include "../engine/npc.h"
#include "../engine/scene_data.h"
#include "../game.h"
#include "../log.h"
#include "../mainloop.h"
#include "../overlay/overlay.h"
#include "../scene.h"
#include "../script/events.h"
#include "freecam.h"
#include "guides.h"
#include "level.h"
#include "mission.h"
#include "model.h"
#include "npc_panel.h"
#include "npcs.h"

namespace h2::editor {

namespace {

bool g_shown;
PlayDebugLayers g_layers;
bool g_panelVisible;

constexpr float kNear = 4000.0f;          // characters drawn within 40 m of the camera
constexpr float kSightDrawn = 4000.0f;    // a field of view drawn at most 40 m long
constexpr double kStillMs = 15000.0;      // a character with a route not moving this long: stuck
constexpr float kStillMoved = 50.0f;      // ... moving less than this (cm)
constexpr double kReactionMs = 8000.0;    // a reaction shown above the head this long
constexpr double kStressMs = 3000.0;      // a suspicion reported shown this long

const ImU32 kWhite = IM_COL32(255, 255, 255, 255), kGrey = IM_COL32(170, 170, 170, 255),
            kYellow = IM_COL32(255, 210, 60, 255), kOrange = IM_COL32(255, 150, 50, 255),
            kRed = IM_COL32(255, 70, 60, 255), kGreen = IM_COL32(110, 230, 110, 255),
            kBlue = IM_COL32(110, 180, 255, 255);

double NowMs() { return double(GetTickCount64()); }

ImU32 Faded(ImU32 c, float a)
{
    return (c & ~IM_COL32_A_MASK) | (ImU32(((c >> IM_COL32_A_SHIFT) & 0xFF) * a) << IM_COL32_A_SHIFT);
}

bool OnMap() { return !CurrentScene().empty() && engine::SceneDataValid() && !engine::SceneRecords().empty(); }

// ---- what happened (the test's log) ----

struct Entry {
    double t = 0.0;  // seconds into the test
    std::string text;
    ImU32 color = kWhite;
    float pos[3] = {};
    bool hasPos = false;
    double at = 0.0;  // when (ms: the corner's latest events)
};
std::vector<Entry> g_log;
bool g_testRunning;              // from a test's start until its scene is left
bool g_waitScene;                // the test's map not playing yet
int g_mapFrames;                 // frames on it since (the level applied)
double g_t0;                     // when its map was played first (ms; 0: not yet)
std::map<std::string, double> g_recent;  // an event of a character kept once in a while (key -> ms)

engine::ZGEOM* ActorOrSelf(engine::ZGEOM* g)
{
    engine::ZGEOM* a = g ? ActorOf(g) : nullptr;
    return a ? a : g;
}

std::string NameOf(engine::ZGEOM* g)
{
    if (!g || !IsLive(g))
        return "someone";
    if (g == engine::Player())
        return "Hitman";
    std::string n = PersonName(g);
    return n.empty() ? engine::SafeString(g->name) : n;
}

void Add(const std::string& text, ImU32 color, engine::ZGEOM* at, const std::string& key = "", double everyMs = 0.0)
{
    if (!g_testRunning || !g_t0)
        return;
    double now = NowMs();
    if (!key.empty()) {
        auto it = g_recent.find(key);
        if (it != g_recent.end() && now - it->second < everyMs)
            return;
        g_recent[key] = now;
    }
    Entry e;
    e.t = (now - g_t0) / 1000.0;
    e.text = text;
    e.color = color;
    e.at = now;
    if (!at || !IsLive(at))
        at = engine::Player();
    if (at && IsLive(at)) {
        Transform t = GetTransform(at);
        memcpy(e.pos, t.pos, sizeof(e.pos));
        e.hasPos = true;
    }
    g_log.push_back(std::move(e));
    if (g_log.size() > 2000)
        g_log.erase(g_log.begin(), g_log.begin() + 500);
}

// ---- what the characters do ----

struct Reaction {
    const char* text = "";
    ImU32 color = kWhite;
    double at = 0.0;
};
std::map<engine::ZGEOM*, Reaction> g_reactions;

struct Still {
    float pos[3] = {};
    double since = 0.0;
    bool reported = false;
};
std::map<engine::ZGEOM*, Still> g_still;
std::map<int, int> g_goalState;  // objective slot -> its state last seen

// the route each walking character goes along as it plays: the points it reached, in that order, where it
// heads now (its movement towards one of them)
struct RouteTrack {
    std::vector<int> reached;  // point numbers (0-based), the latest last (at most 12)
    int heading = -1;
    float last[3] = {};
    bool hasLast = false;
    double reportedAt = -1e9;
};
std::map<engine::ZGEOM*, RouteTrack> g_routes;
double g_lastTrack;
constexpr float kReached = 130.0f;  // a point reached within this (cm, flat)
bool g_goalsSeen;
bool g_hitmanDead;
double g_lastPoll;

// a victim as the level routine got it: a reference or the character itself
engine::ZGEOM* Victim(const engine::GameEvent& e)
{
    if (!e.hasValue || !e.value)
        return nullptr;
    if (engine::ZGEOM* g = engine::GeomOfRef(e.value))
        return g;
    for (engine::ZGEOM* c : PlayingCharacters())
        if (uint32_t(reinterpret_cast<uintptr_t>(c)) == e.value)
            return c;
    return nullptr;
}

std::string Lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void OnEvent(const char* event, const engine::GameEvent& e)
{
    std::string ev = event;
    struct Kind {
        const char* event;
        const char* above;  // above the head
        const char* log;    // "<name> ..."
        ImU32 color;
    };
    static const Kind kReactions[] = {
        {"suspicious", "suspicious", "is suspicious", kYellow},
        {"recognized", "sees through the disguise", "sees through the disguise", kOrange},
        {"hostile", "hostile", "turns hostile", kOrange},
        {"spotted", "SPOTTED HITMAN", "spotted Hitman", kRed},
        {"help", "calls for help", "calls for help", kRed},
    };
    for (const Kind& k : kReactions) {
        if (ev != k.event)
            continue;
        engine::ZGEOM* who = ActorOrSelf(e.to);
        if (!who)
            return;
        g_reactions[who] = {k.above, k.color, NowMs()};
        char key[64];
        snprintf(key, sizeof(key), "%s:%p", k.event, static_cast<void*>(who));
        Add(NameOf(who) + " " + k.log, k.color, who, key, 10000.0);
        return;
    }
    if (ev == "alarm") {
        std::string n = Lower(e.name);
        bool off = n.find("leave") != std::string::npos || n.find("end") != std::string::npos;
        Add(off ? "The alarm is over" : "Alarm!", off ? kGreen : kRed, engine::Player(), off ? "alarm0" : "alarm1",
            5000.0);
    } else if (ev == "kill" || ev == "harm" || ev == "unconscious") {
        engine::ZGEOM* v = ActorOrSelf(ev == "unconscious" ? (Victim(e) ? Victim(e) : e.to) : Victim(e));
        if (!v || v == engine::Player())
            return;
        bool killed = ev == "kill";
        const char* kind = e.type == 1 ? " (an innocent)" : "";
        char key[64];
        snprintf(key, sizeof(key), "%s:%p", killed ? "kill" : "out", static_cast<void*>(v));
        Add(NameOf(v) + (killed ? " killed" : " knocked out") + (killed ? kind : ""), killed ? kRed : kOrange, v,
            key, 30000.0);
    } else if (ev == "disguise") {
        Add("Hitman changed clothes", kBlue, engine::Player(), "disguise", 3000.0);
    } else if (ev == "pickup") {
        Add("Hitman picked something up", kWhite, engine::Player(), "pickup", 2000.0);
    } else if (ev == "mission") {
        bool done = e.kind == engine::GameEvent::MissionCompleted;
        Add(std::string(done ? "Mission completed" : "Mission failed") +
                (e.type == 1 ? " (the map's own end: stopped)" : ""),
            done ? kGreen : kRed, engine::Player(), done ? "mission1" : "mission0", 2000.0);
    }
}

// the state of a behaviour as a phrase ("Being2.WalkRoute" -> "walk route")
std::string Phrase(const std::string& state)
{
    size_t dot = state.find_last_of(".:");
    std::string s = dot == std::string::npos ? state : state.substr(dot + 1);
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '_') {
            out += ' ';
            continue;
        }
        if (i && std::isupper(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(s[i - 1])))
            out += ' ';
        out += char(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// knocked out: the level routine told so, or its behaviour is in the base class's state (Being2.Unconscious:
// a knock-out is not always reported, mission.cpp)
bool KnockedOut(engine::ZGEOM* actor)
{
    if (engine::WasKilled(actor))
        return false;
    return engine::WasHarmed(actor) || Lower(engine::ScriptState(actor)).find("unconscious") != std::string::npos;
}

// every quarter second while a test plays: the objectives, Hitman's death, characters standing still
void Poll()
{
    if (g_waitScene) {  // (the scene's push comes inside the request already: its map plays once applied)
        g_mapFrames = OnMap() && !LevelApplyPending() ? g_mapFrames + 1 : 0;
        if (g_mapFrames < 30)
            return;
        g_waitScene = false;
    }
    if (!g_testRunning || !OnMap() || overlay::IsOpen())
        return;
    double now = NowMs();
    if (!g_t0) {
        g_t0 = now;
        Add("The test starts", kWhite, engine::Player());
    }
    if (now - g_lastPoll < 250.0)
        return;
    g_lastPoll = now;
    for (const GoalStatus& g : GoalStatuses()) {
        auto it = g_goalState.find(g.slot);
        int before = it == g_goalState.end() ? 0 : it->second;
        g_goalState[g.slot] = g.state;
        if (g.state == before || (!g_goalsSeen && g.state == 1))
            continue;  // (done from the start: a "never spotted" one)
        char head[32];
        snprintf(head, sizeof(head), "Objective %d ", g.slot);
        if (g.state == 1)
            Add(head + std::string("done: ") + g.text, kGreen, g.where);
        else if (g.state == -1)
            Add(head + std::string("lost: ") + g.text + " (" + g.why + ")", kRed, g.where);
    }
    g_goalsSeen = true;
    engine::ZGEOM* hitman = engine::Player();
    bool dead = hitman && !engine::IsAlive(hitman);
    if (dead && !g_hitmanDead)
        Add("Hitman died", kRed, hitman);
    g_hitmanDead = dead;
    for (engine::ZGEOM* a : PlayingCharacters()) {
        if (!WalksARoute(a) || !engine::IsAlive(a) || KnockedOut(a)) {
            g_still.erase(a);
            continue;
        }
        Transform t = GetTransform(a);
        auto it = g_still.find(a);
        if (it == g_still.end()) {
            Still s;
            memcpy(s.pos, t.pos, sizeof(s.pos));
            s.since = now;
            g_still[a] = s;
            continue;
        }
        Still& s = it->second;
        float d[3] = {t.pos[0] - s.pos[0], t.pos[1] - s.pos[1], t.pos[2] - s.pos[2]};
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > kStillMoved * kStillMoved) {
            memcpy(s.pos, t.pos, sizeof(s.pos));
            s.since = now;
            s.reported = false;
        } else if (!s.reported && now - s.since > kStillMs) {
            s.reported = true;
            // why: the next point of its route (in order) cannot be reached
            std::string why = "its route?";
            WalkedRoute r;
            if (WalkedRouteOf(a, r)) {
                auto t = g_routes.find(a);
                int next = t == g_routes.end() || t->second.reached.empty()
                               ? 0
                               : (t->second.reached.back() + 1) % int(r.points.size());
                if (engine::OnPathNetwork(GetTransform(r.points[next]).pos) == 0)
                    why = "its route point " + std::to_string(next + 1) + " is off the map's paths: it never gets there";
                else if (!r.order.why.empty())
                    why = r.behaviour + ", " + r.order.why;
            }
            Add(NameOf(a) + " has not moved for 15 s (" + why + ")", kOrange, a);
        }
    }
}


// every 150 ms while the level plays: which route point each walking character reached or heads to; a
// point reached out of order is logged with why (its behaviour's way of walking)
void TrackRoutes()
{
    if (!OnMap() || overlay::IsOpen() || g_waitScene)
        return;
    double now = NowMs();
    if (now - g_lastTrack < 150.0)
        return;
    g_lastTrack = now;
    for (engine::ZGEOM* a : PlayingCharacters()) {
        WalkedRoute r;
        if (!engine::IsAlive(a) || KnockedOut(a) || !WalkedRouteOf(a, r)) {
            g_routes.erase(a);
            continue;
        }
        RouteTrack& t = g_routes[a];
        Transform at = GetTransform(a);
        int n = int(r.points.size()), atPoint = -1;
        float best = kReached * kReached;
        std::vector<Transform> pts;
        for (engine::ZGEOM* g : r.points)
            pts.push_back(GetTransform(g));
        for (int k = 0; k < n; ++k) {
            float dx = pts[k].pos[0] - at.pos[0], dz = pts[k].pos[2] - at.pos[2], dy = pts[k].pos[1] - at.pos[1];
            float d = dx * dx + dz * dz;
            if (d < best && std::fabs(dy) < 250.0f) {
                best = d;
                atPoint = k;
            }
        }
        if (atPoint >= 0 && (t.reached.empty() || t.reached.back() != atPoint)) {
            int before = t.reached.empty() ? -1 : t.reached.back();
            t.reached.push_back(atPoint);
            if (t.reached.size() > 12)
                t.reached.erase(t.reached.begin());
            int expected = before < 0 ? 0 : (before + 1) % n;
            bool back = before >= 0 && atPoint == before - 1;  // (back and forth)
            if (atPoint != expected && !back && now - t.reportedAt > 20000.0 && n > 1) {
                t.reportedAt = now;
                char text[200];
                snprintf(text, sizeof(text), "%s reached route point %d %s (in order: %d)", NameOf(a).c_str(),
                         atPoint + 1, before < 0 ? "first" : ("after " + std::to_string(before + 1)).c_str(),
                         expected + 1);
                std::string why = !r.order.why.empty() ? r.order.why
                                  : before < 0         ? "it started nearer to that point"
                                                       : "a point in between not reached (off the paths? in the way?)";
                Add(std::string(text) + ": " + r.behaviour + ", " + why, kOrange, a);
            }
        }
        // heading: the point its movement points at
        if (t.hasLast) {
            float v[3] = {at.pos[0] - t.last[0], 0.0f, at.pos[2] - t.last[2]};
            float len = std::sqrt(v[0] * v[0] + v[2] * v[2]);
            if (len > 5.0f) {
                float bestCos = 0.85f;
                int head = -1;
                for (int k = 0; k < n; ++k) {
                    float w[3] = {pts[k].pos[0] - at.pos[0], 0.0f, pts[k].pos[2] - at.pos[2]};
                    float wl = std::sqrt(w[0] * w[0] + w[2] * w[2]);
                    if (wl < kReached)
                        continue;
                    float c = (v[0] * w[0] + v[2] * w[2]) / (len * wl);
                    if (c > bestCos) {
                        bestCos = c;
                        head = k;
                    }
                }
                t.heading = head;
            }
        }
        memcpy(t.last, at.pos, sizeof(t.last));
        t.hasLast = true;
    }
}

// ---- drawing ----

// how far and how wide a seer sees: a fan on the floor (red while it sees Hitman), the 2 m all round
void DrawSight(ImDrawList* dl, const engine::Seer& s, float a)
{
    float base[3] = {s.eye[0], s.eye[1], s.eye[2]};
    if (s.actor)
        base[1] = GetTransform(s.geom).pos[1] + 5.0f;  // (on the floor under the eye)
    ImU32 c = s.seesHitman ? kRed : kYellow;
    ImU32 fill = Faded(c, (s.seesHitman ? 0.30f : 0.16f) * a), edge = Faded(c, 0.85f * a);
    float hx = s.dir[0], hz = s.dir[2];
    float len = std::sqrt(hx * hx + hz * hz);
    float half = std::acos(std::clamp(s.cosHalf, -1.0f, 1.0f));
    float r = std::min(s.range, kSightDrawn);
    auto at = [&](float ang, float rad, float out[3]) {
        out[0] = base[0] + rad * std::sin(ang);
        out[1] = base[1];
        out[2] = base[2] + rad * std::cos(ang);
    };
    constexpr int kArc = 24;
    if (len > 0.01f && half > 0.01f) {
        float yaw = std::atan2(hx / len, hz / len);
        ImVec2 p0;
        bool b = WorldToScreen(base, p0);
        float prev[3];
        ImVec2 sp;
        bool sOk = false;
        ImDrawListFlags flags = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;  // (no seams between the triangles)
        for (int i = 0; i <= kArc; ++i) {
            float p[3];
            at(yaw - half + 2.0f * half * float(i) / kArc, r, p);
            ImVec2 q;
            bool ok = WorldToScreen(p, q);
            if (i > 0) {
                if (b && ok && sOk)
                    dl->AddTriangleFilled(p0, sp, q, fill);
                DrawLine3D(prev, p, edge, 2.0f);
            }
            memcpy(prev, p, sizeof(prev));
            sp = q;
            sOk = ok;
        }
        dl->Flags = flags;
        if (half < 3.1f) {
            float p[3];
            at(yaw - half, r, p);
            DrawLine3D(base, p, edge, 2.0f);
            at(yaw + half, r, p);
            DrawLine3D(base, p, edge, 2.0f);
        }
        float tip[3];
        at(yaw, r, tip);
        char t[48];
        snprintf(t, sizeof(t), s.range > kSightDrawn ? "sees %.0f m (and farther)" : "sees %.0f m",
                 std::min(s.range, kSightDrawn) / 100.0f);
        DrawLabel3D(tip, t, edge);
    }
    float prev[3];
    for (int i = 0; i <= kArc; ++i) {  // the near ring: seen at any angle
        float p[3];
        at(2.0f * 3.14159265f * float(i) / kArc, engine::kSeeNear, p);
        if (i > 0)
            DrawLine3D(prev, p, Faded(c, 0.5f * a), 1.5f);
        memcpy(prev, p, sizeof(prev));
    }
}

// a line of text centred under `y`, on a dark box; returns the next line's y
float Line(ImDrawList* dl, float x, float y, const char* text, ImU32 color, float a)
{
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 p(x - ts.x * 0.5f, y);
    dl->AddRectFilled(ImVec2(p.x - 4, p.y - 1), ImVec2(p.x + ts.x + 4, p.y + ts.y + 1), IM_COL32(0, 0, 0, int(150 * a)),
                      3.0f);
    dl->AddText(p, Faded(color, a), text);
    return y + ts.y + 3.0f;
}

// under a walking character's name: how it walks its route, the points it reached, where it heads (a line
// to that point)
float DrawRouteInfo(ImDrawList* dl, engine::ZGEOM* actor, float x, float y, float a)
{
    WalkedRoute r;
    if (!WalkedRouteOf(actor, r))
        return y;
    auto it = g_routes.find(actor);
    std::string line = "route of " + std::to_string(r.points.size());
    if (it != g_routes.end() && !it->second.reached.empty()) {
        line += ": reached";
        size_t from = it->second.reached.size() > 6 ? it->second.reached.size() - 6 : 0;
        for (size_t i = from; i < it->second.reached.size(); ++i)
            line += " " + std::to_string(it->second.reached[i] + 1);
    }
    if (it != g_routes.end() && it->second.heading >= 0 && it->second.heading < int(r.points.size())) {
        line += ", to " + std::to_string(it->second.heading + 1);
        Transform at = GetTransform(actor), to = GetTransform(r.points[it->second.heading]);
        float from[3] = {at.pos[0], at.pos[1] + 20.0f, at.pos[2]}, dst[3] = {to.pos[0], to.pos[1] + 20.0f, to.pos[2]};
        DrawLine3D(from, dst, Faded(kWhite, 0.6f * a), 1.5f);
    }
    y = Line(dl, x, y, line.c_str(), IM_COL32(200, 230, 255, 255), a);
    if (!r.order.inOrder || !r.order.why.empty())
        y = Line(dl, x, y, (r.behaviour + ": " + r.order.why).c_str(), r.order.inOrder ? kYellow : kOrange, a);
    return y;
}

void DrawDoing(ImDrawList* dl, engine::ZGEOM* actor, float a, bool sees, double now)
{
    Transform at = GetTransform(actor);
    float head[3] = {at.pos[0], at.pos[1] + 200.0f, at.pos[2]};
    ImVec2 s;
    if (!WorldToScreen(head, s))
        return;
    float y = s.y + 7.0f;  // under the name the routes draw (npc_panel.cpp DrawPlayRoutes)
    if (!(g_layers.routes && WalksARoute(actor))) {
        std::string name = NameOf(actor);
        ImVec2 ts = ImGui::CalcTextSize(name.c_str());
        Line(dl, s.x, s.y - ts.y, name.c_str(), kWhite, a);
    }
    if (!engine::IsAlive(actor) || KnockedOut(actor)) {
        Line(dl, s.x, y, KnockedOut(actor) ? "knocked out" : "dead", kGrey, a);
        return;
    }
    if (g_layers.routes)
        y = DrawRouteInfo(dl, actor, s.x, y, a);
    std::string state = Phrase(engine::ScriptState(actor));
    if (!state.empty())
        y = Line(dl, s.x, y, state.c_str(), IM_COL32(220, 220, 220, 255), a);
    auto r = g_reactions.find(actor);
    if (r != g_reactions.end() && now - r->second.at < kReactionMs)
        y = Line(dl, s.x, y, r->second.text, r->second.color, a);
    int stress = 0;
    double age = 0.0;
    if (engine::ReportedStress(actor, stress, age) && age < kStressMs && stress > 0) {
        char t[32];
        snprintf(t, sizeof(t), "suspicion %d", stress);
        ImVec2 ts = ImGui::CalcTextSize(t);
        float w = std::max(ts.x, 80.0f), x0 = s.x - w * 0.5f;
        float f = std::clamp(stress / 100.0f, 0.0f, 1.0f);
        ImU32 c = IM_COL32(int(110 + 145 * f), int(230 - 160 * f), 60, 255);
        dl->AddRectFilled(ImVec2(x0 - 4, y - 1), ImVec2(x0 + w + 4, y + ts.y + 7), IM_COL32(0, 0, 0, int(150 * a)), 3.0f);
        dl->AddText(ImVec2(s.x - ts.x * 0.5f, y), Faded(c, a), t);
        dl->AddRectFilled(ImVec2(x0, y + ts.y + 2), ImVec2(x0 + w * f, y + ts.y + 5), Faded(c, a));
        y += ts.y + 10.0f;
    }
    if (sees)
        y = Line(dl, s.x, y, "sees Hitman", kRed, a);
    if (g_layers.stuck) {
        auto st = g_still.find(actor);
        if (st != g_still.end() && now - st->second.since > kStillMs) {
            char t[48];
            snprintf(t, sizeof(t), "not moving for %.0f s", (now - st->second.since) / 1000.0);
            Line(dl, s.x, y, t, kOrange, a);
        }
    }
}

// the corner: the alarm level, the objectives, the latest events
void DrawCorner(ImDrawList* dl, double now)
{
    ImGuiIO& io = ImGui::GetIO();
    float lineH = ImGui::GetTextLineHeight() + 3.0f;
    float w = std::min(380.0f, io.DisplaySize.x * 0.32f), x = io.DisplaySize.x - w - 16.0f, y = 70.0f;
    std::vector<GoalStatus> goals = GoalStatuses();
    std::vector<const Entry*> latest;
    for (auto it = g_log.rbegin(); it != g_log.rend() && latest.size() < 3; ++it)
        if (now - it->at < 6000.0)
            latest.push_back(&*it);
    int stress = engine::LevelStress();
    float h = lineH * (1 + float(goals.size()) + (goals.empty() ? 0 : 1) + float(latest.size())) + 14.0f;
    dl->AddRectFilled(ImVec2(x - 8, y - 6), ImVec2(x + w + 8, y + h), IM_COL32(0, 0, 0, 140), 5.0f);
    char t[160];
    // the level's alarm level (the HUD's stress meter): above 50 the music changes
    float f = std::clamp((stress < 0 ? 0 : stress) / 100.0f, 0.0f, 1.0f);
    snprintf(t, sizeof(t), stress < 0 ? "Alarm level -" : "Alarm level %d", stress);
    ImU32 sc = IM_COL32(int(110 + 145 * f), int(230 - 160 * f), 60, 255);
    dl->AddText(ImVec2(x, y), sc, t);
    float bx = x + ImGui::CalcTextSize("Alarm level 100  ").x;
    dl->AddRectFilled(ImVec2(bx, y + lineH * 0.35f), ImVec2(x + w, y + lineH * 0.6f), IM_COL32(255, 255, 255, 40));
    dl->AddRectFilled(ImVec2(bx, y + lineH * 0.35f), ImVec2(bx + (x + w - bx) * f, y + lineH * 0.6f), sc);
    y += lineH;
    if (!goals.empty()) {
        dl->AddText(ImVec2(x, y), kGrey, "Objectives");
        y += lineH;
        for (const GoalStatus& g : goals) {
            const char* mark = g.state > 0 ? "[x]" : g.state < 0 ? "[!]" : "[ ]";
            ImU32 c = g.state > 0 ? kGreen : g.state < 0 ? kRed : kWhite;
            snprintf(t, sizeof(t), "%s %d %s", mark, g.slot, g.text.c_str());
            std::string why = g.why.empty() ? "" : "  " + g.why;
            float ww = ImGui::CalcTextSize(why.c_str()).x;
            dl->PushClipRect(ImVec2(x, y), ImVec2(x + w - ww - 6, y + lineH), true);
            dl->AddText(ImVec2(x, y), c, t);
            dl->PopClipRect();
            dl->AddText(ImVec2(x + w - ww, y), g.state < 0 ? kRed : kGrey, why.c_str());
            y += lineH;
        }
    }
    for (auto it = latest.rbegin(); it != latest.rend(); ++it) {
        const Entry* e = *it;
        float fade = std::clamp(1.0f - float(now - e->at - 4000.0) / 2000.0f, 0.0f, 1.0f);
        snprintf(t, sizeof(t), "%d:%02d  %s", int(e->t) / 60, int(e->t) % 60, e->text.c_str());
        dl->PushClipRect(ImVec2(x, y), ImVec2(x + w, y + lineH), true);
        dl->AddText(ImVec2(x, y), Faded(e->color, fade), t);
        dl->PopClipRect();
        y += lineH;
    }
}

void Draw()
{
    if (!g_shown || overlay::IsOpen() || !OnMap())
        return;
    game::ZCAMERA* cam = game::MainCamera();
    if (!cam)
        return;
    float cm[9], cp[3];
    game::GetWorldTransform(cam, cm, cp);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    double now = NowMs();
    auto fade = [&](const float p[3]) {
        float dx = p[0] - cp[0], dz = p[2] - cp[2];
        float d = std::sqrt(dx * dx + dz * dz);
        return d > kNear ? 0.0f : std::clamp(1.0f - (d - 2000.0f) / 3000.0f, 0.4f, 1.0f);
    };
    std::set<engine::ZGEOM*> seeing;
    if (g_layers.doing || g_layers.sight) {
        engine::ZGEOM* hitman = engine::Player();
        for (const engine::Seer& s : engine::Seers()) {
            if (s.geom == hitman || (s.actor && (!engine::IsAlive(s.geom) || KnockedOut(s.geom))))
                continue;
            if (s.seesHitman)
                seeing.insert(s.geom);
            float a = fade(s.eye);
            if (g_layers.sight && a > 0.0f)
                DrawSight(dl, s, a);
        }
    }
    for (engine::ZGEOM* actor : PlayingCharacters()) {
        float a = fade(GetTransform(actor).pos);
        if (a <= 0.0f)
            continue;
        if (g_layers.doing) {
            DrawDoing(dl, actor, a, seeing.count(actor) > 0, now);
        } else if (g_layers.routes) {  // (the route's line under the name the routes draw)
            Transform at = GetTransform(actor);
            float head[3] = {at.pos[0], at.pos[1] + 200.0f, at.pos[2]};
            ImVec2 sp;
            if (WorldToScreen(head, sp))
                DrawRouteInfo(dl, actor, sp.x, sp.y + 7.0f, a);
        }
    }
    if (g_layers.goals)
        DrawCorner(dl, now);
}

bool OnKey(int vk)
{
    if (vk != g_config.routesKey || !vk || !OnMap())
        return false;
    g_shown = !g_shown;
    return true;
}

void DrawPanel() { DrawLastTest(); }

} // namespace

bool* PlayDebugShown() { return &g_shown; }
PlayDebugLayers& PlayDebugView() { return g_layers; }
bool LastTestHasEvents() { return !g_log.empty(); }

std::string LastTestText()
{
    std::string out;
    for (const Entry& e : g_log) {
        char t[16];
        snprintf(t, sizeof(t), "%d:%02d  ", int(e.t) / 60, int(e.t) % 60);
        out += t + e.text + "\n";
    }
    return out;
}

void DrawPlayDebugSettings()
{
    std::string label = "Debug view while playing (" + overlay::KeyNameOf(g_config.routesKey) + ")";
    ImGui::Checkbox(label.c_str(), &g_shown);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shown while the level is played (the editor closed); the key switches it on and off.");
    ImGui::Indent();
    ImGui::Checkbox("Routes", &g_layers.routes);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Where each person walks: numbered points, arrows in walking order, the name above\n"
                          "the head in the same colour (red point: off the paths, never reached).");
    ImGui::Checkbox("What they do", &g_layers.doing);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Under each name (within 40 m): what the person is doing (the state of its behaviour),\n"
                          "how it reacts (suspicious, sees through the disguise, hostile, spotted Hitman),\n"
                          "the suspicion it reports (the alarm level is the largest), and \"sees Hitman\".");
    ImGui::Checkbox("What they see", &g_layers.sight);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How far and how wide each one sees, on the floor (red: sees Hitman now), as the game\n"
                          "checks it. The small ring: 2 m, seen at any angle. Seen also needs a clear line of\n"
                          "sight and enough light.");
    ImGui::Checkbox("Objectives and alarm level", &g_layers.goals);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("In the corner: the alarm level (the game's stress meter, the music changes above 50),\n"
                          "each objective (done, not yet and how far, lost and why), the latest events.");
    ImGui::Checkbox("Stuck people", &g_layers.stuck);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A person with a route that has not moved for 15 s (stopped on its way: a point it cannot\n"
                          "reach, a door, someone in the way). Waiting at a point is fine for some behaviours.");
    ImGui::Unindent();
}

void DrawLastTest()
{
    if (g_log.empty()) {
        ImGui::TextDisabled("Nothing yet: play the level (Try it), then come back (%s).",
                            overlay::EditorKeyName().c_str());
        return;
    }
    ImGui::TextDisabled("%s%zu events. Click one: the camera goes there.", g_testRunning ? "(still playing) " : "",
                        g_log.size());
    float h = std::min(ImGui::GetTextLineHeightWithSpacing() * float(g_log.size() + 1),
                       ImGui::GetTextLineHeightWithSpacing() * 16.0f);
    if (ImGui::BeginChild("lasttest", ImVec2(-1, h), ImGuiChildFlags_Borders)) {
        for (size_t i = 0; i < g_log.size(); ++i) {
            const Entry& e = g_log[i];
            char row[200];
            snprintf(row, sizeof(row), "%d:%02d  %s##%zu", int(e.t) / 60, int(e.t) % 60, e.text.c_str(), i);
            ImVec4 c = ImGui::ColorConvertU32ToFloat4(e.color);
            ImGui::PushStyleColor(ImGuiCol_Text, c);
            if (ImGui::Selectable(row) && e.hasPos)
                FlyCameraTo(e.pos, 700.0f);
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
}

void PlayDebugTestStarted()
{
    g_log.clear();
    g_recent.clear();
    g_goalState.clear();
    g_goalsSeen = false;
    g_hitmanDead = false;
    g_t0 = 0.0;
    g_testRunning = true;
    g_waitScene = true;  // (the played map was left in the request already)
    g_mapFrames = 0;
}

void PlayDebugBackInEditor()
{
    if (g_log.empty())
        return;
    if (!overlay::SimpleMode()) {
        g_panelVisible = true;
        overlay::ShowPanel("Last test");
    }
}

void InstallPlayDebug()
{
    script::SetEditorEvents({"suspicious", "recognized", "hostile", "spotted", "help", "alarm", "unconscious",
                             "disguise", "pickup", "kill", "harm", "mission"},
                            OnEvent);
    overlay::AddPanel({"Last test", &g_panelVisible, DrawPanel, overlay::Dock::Right});
    overlay::AddAlwaysDraw(Draw);
    overlay::AddKeyHandler(OnKey);
    OnFrameEnd(Poll);
    OnFrameEnd(TrackRoutes);
    OnSceneLeaving([](const std::string&) {
        g_reactions.clear();
        g_routes.clear();
        g_still.clear();
        if (!g_waitScene)
            g_testRunning = false;
    });
}

} // namespace h2::editor
