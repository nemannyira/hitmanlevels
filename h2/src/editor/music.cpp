#include "music.h"

#include <windows.h>
#include <algorithm>
#include <cstring>
#include <map>

#include "imgui.h"

#include "../engine/scene_data.h"
#include "../log.h"
#include "../scene.h"
#include "level.h"
#include "model.h"

namespace h2::editor {

namespace {

constexpr char kMusicDir[] = "Streams/Music/";

std::vector<MusicTrack> g_tracks;
std::string g_tracksFor;                     // scene the tracks were read for
ULONGLONG g_nextScan;                        // not found: when to look again
std::map<std::string, std::string> g_choice;  // music object path -> track chosen ("" = the level's)

uint32_t U32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

// The game's music (engine/eventsbase/soundc): a zone's box has a MusicSegment
// routine (vtable 0x64B858), whose data load (0x4A7F10) keeps its mood at +0x72
// and its sound reference at +0x7A (saved with the game, 0x4A80B0). The
// MusicControl group's routine (ctor 0x4A81B0) checks every 0x400 ms which of
// its rooms Hitman is in (0x4A86B0 -> 0x4A8720); entering one, it takes each
// segment's state as it is then (segment vfunc 0x84, 0x4A7FC0) and sends them
// to the level's LevelControl (0x4A8470 -> geom vfunc 0x118), which plays them.
// Its flag +0x1E4 (set by its ctor, cleared after a check) makes the next check
// take the room as entered anew.
constexpr uint32_t kSegment_Sound = 0x7A;
constexpr uint32_t kControl_CheckAnew = 0x1E4;


void ReadTracks();

const uint8_t* TableOf(const uint8_t* name, const uint8_t* regionEnd)
{
    size_t len = strnlen(reinterpret_cast<const char*>(name), size_t(regionEnd - name));
    const uint8_t* fields = name + len + 1;
    if (fields + 16 > regionEnd || U32(fields) != 1)
        return nullptr;
    uint32_t nameOffset = U32(fields + 4);
    if (nameOffset < 16 || nameOffset > uintptr_t(name))
        return nullptr;
    const uint8_t* base = name - nameOffset;
    if (!engine::IsReadable(base, 16) || U32(base + 8) != 3 || U32(base + 12) != 4)
        return nullptr;
    return base;
}

const uint8_t* ScanRegion(const uint8_t* begin, const uint8_t* end)
{
    __try {
        const size_t n = sizeof(kMusicDir) - 1;
        for (const uint8_t* p = begin; p + n < end;) {
            p = static_cast<const uint8_t*>(memchr(p, 'S', size_t(end - p - n)));
            if (!p)
                break;
            if (memcmp(p, kMusicDir, n) == 0)
                if (const uint8_t* base = TableOf(p, end))
                    return base;
            ++p;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

// the game's copy of the level's sound table (.SND)
const uint8_t* FindSoundTable()
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    auto a = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    auto last = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mi;
    while (a < last && VirtualQuery(reinterpret_cast<void*>(a), &mi, sizeof(mi))) {
        uintptr_t next = reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
        bool readable = mi.State == MEM_COMMIT && mi.Type == MEM_PRIVATE && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                        (mi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE));
        if (readable)
            if (const uint8_t* base = ScanRegion(static_cast<const uint8_t*>(mi.BaseAddress),
                                                 reinterpret_cast<const uint8_t*>(next)))
                return base;
        if (next <= a)
            break;
        a = next;
    }
    return nullptr;
}

// tracks of the sound table: name and the reference music objects use
void ReadTracks()
{
    g_tracks.clear();
    if (GetTickCount64() < g_nextScan)
        return;
    const uint8_t* base = FindSoundTable();
    if (!base) {
        g_nextScan = GetTickCount64() + 3000;
        return;
    }
    g_tracksFor = CurrentScene();
    uint32_t size = U32(base + 4);
    if (size < 32 || size > (8u << 20) || !engine::IsReadable(base, size))
        return;
    const uint8_t* end = base + size;
    const size_t n = sizeof(kMusicDir) - 1;
    for (const uint8_t* p = base + 16; p + n < end; ++p) {
        if (*p != 'S' || memcmp(p, kMusicDir, n) != 0)
            continue;
        size_t len = strnlen(reinterpret_cast<const char*>(p), size_t(end - p));
        const uint8_t* fields = p + len + 1;
        if (fields + 16 > end || U32(fields) != 1 || U32(fields + 4) != uint32_t(p - base))
            continue;
        std::string name(reinterpret_cast<const char*>(p) + n, len - n);
        if (name.size() > 4 && _stricmp(name.c_str() + name.size() - 4, ".wav") == 0)
            name.resize(name.size() - 4);
        g_tracks.push_back({name, uint32_t(fields - base)});
        p = fields + 15;
    }
    Log("music: %d tracks in the sound table", int(g_tracks.size()));
}

// the first sound reference (tag 0x0B) of a data block: its offset in the block
size_t SoundRefAt(const std::vector<uint8_t>& b)
{
    size_t i = 4;
    while (i < b.size() && b[i] != 0xFF) {
        uint8_t tag = b[i] & 0x3F;
        if (tag == 0x0B)
            return i + 1;
        if (tag == 7 || tag == 9 || tag == 0xC) {
            if (i + 5 > b.size())
                return 0;
            i += 1 + U32(&b[i + 1]);
        } else if (tag == 1) {
            i += 9;
        } else if (tag == 2 || tag == 3 || tag == 8 || tag == 10) {
            i += 5;
        } else if (tag == 4 || tag == 5) {
            ++i;
            while (i < b.size() && b[i])
                ++i;
            ++i;
        } else {
            ++i;
        }
    }
    return 0;
}

bool InGroup(const std::vector<engine::SceneRecord>& records, int r, int group)
{
    for (int depth = 0; r >= 0 && depth < 32; ++depth) {
        if (r == group)
            return true;
        r = records[r].parent;
    }
    return false;
}

// A segment's sound as the game's own load gives it, and its MusicControl
// told to take the room Hitman is in as entered anew (its segments sent again).
bool SetSegmentSound(const MusicSlot& s, uint32_t ref)
{
    uint8_t* segment = engine::GeomRoutine(engine::RecordGeom(s.record), "MusicSegment");
    if (!segment)
        return false;
    memcpy(segment + kSegment_Sound, &ref, 4);
    const auto& records = engine::SceneRecords();
    for (int r = records[s.record].parent; r >= 0; r = records[r].parent)
        if (records[r].name == "MusicControl") {
            if (uint8_t* control = engine::GeomRoutine(engine::RecordGeom(r), "MusicControl"))
                control[kControl_CheckAnew] = 1;
            break;
        }
    return true;
}

} // namespace

const std::vector<MusicTrack>& MusicTracks()
{
    if (g_tracksFor != CurrentScene() && engine::SceneDataValid())
        ReadTracks();
    return g_tracks;
}

std::string TrackName(uint32_t ref)
{
    for (const MusicTrack& t : MusicTracks())
        if (t.ref == ref)
            return t.name;
    char buf[32];
    snprintf(buf, sizeof(buf), "sound %X", ref);
    return buf;
}

std::vector<MusicSlot> MusicSlots()
{
    std::vector<MusicSlot> out;
    if (!engine::SceneDataValid())
        return out;
    const auto& records = engine::SceneRecords();
    for (size_t g = 0; g < records.size(); ++g) {
        if (records[g].name != "MusicControl")
            continue;
        for (size_t r = g + 1; r < records.size(); ++r) {
            if (!records[r].data || !InGroup(records, int(r), int(g)))
                continue;
            std::vector<uint8_t> b = engine::RemappedBlock(records[r].data);
            size_t at = SoundRefAt(b);
            if (!at)
                continue;
            MusicSlot s;
            s.record = int(r);
            engine::ZGEOM* geom = engine::RecordGeom(int(r));
            s.path = geom && IsLive(geom) ? PathOf(geom) : "";
            if (s.path.empty())
                continue;
            s.zone = records[r].parent >= 0 ? records[records[r].parent].name : "";
            s.mood = records[r].name.substr(0, records[r].name.find('_'));
            s.original = U32(&b[at]);
            s.current = s.original;
            auto it = g_choice.find(s.path);
            if (it != g_choice.end() && !it->second.empty())
                for (const MusicTrack& t : MusicTracks())
                    if (_stricmp(t.name.c_str(), it->second.c_str()) == 0)
                        s.current = t.ref;
            out.push_back(s);
        }
    }
    return out;
}

bool SetMusicTrack(const std::string& slotPath, const std::string& track, std::string* error)
{
    bool found = false;
    for (const MusicSlot& s : MusicSlots())
        found |= s.path == slotPath;
    if (!found) {
        if (error)
            *error = "no such music object in this scene";
        return false;
    }
    if (!track.empty()) {
        bool known = false;
        for (const MusicTrack& t : MusicTracks())
            known |= _stricmp(t.name.c_str(), track.c_str()) == 0;
        if (!known) {
            if (error)
                *error = "no music track " + track + " in this level";
            return false;
        }
    }
    for (const MusicSlot& s : MusicSlots())
        if (s.path == slotPath) {
            uint32_t ref = s.original;
            for (const MusicTrack& t : MusicTracks())
                if (!track.empty() && _stricmp(t.name.c_str(), track.c_str()) == 0)
                    ref = t.ref;
            if (!SetSegmentSound(s, ref)) {
                if (error)
                    *error = "the music object has no MusicSegment routine";
                return false;
            }
        }
    g_choice[slotPath] = track;
    Log("music: %s -> %s", slotPath.c_str(), track.empty() ? "(the level's)" : track.c_str());
    return true;
}

void DrawMusicSection()
{
    std::vector<MusicSlot> slots = MusicSlots();
    if (slots.empty()) {
        ImGui::TextDisabled("this level has no music objects");
        return;
    }
    const std::vector<MusicTrack>& tracks = MusicTracks();
    if (tracks.empty()) {
        ImGui::TextDisabled("the level's sound table was not found");
        return;
    }
    LevelDocument& L = CurrentLevel();
    std::string zone;
    for (const MusicSlot& s : slots) {
        if (s.zone != zone) {
            zone = s.zone;
            ImGui::TextDisabled("%s", zone.c_str());
        }
        ImGui::PushID(s.path.c_str());
        const char* label = s.mood == "Location"    ? "calm"
                            : s.mood == "PreAction" ? "tension"
                            : s.mood == "Action"    ? "combat"
                                                    : s.mood.c_str();
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize("tension").x - ImGui::GetStyle().ItemSpacing.x * 2);
        std::string current = TrackName(s.current) + (s.current == s.original ? " (level)" : "");
        if (ImGui::BeginCombo(label, current.c_str())) {
            for (const MusicTrack& t : tracks) {
                std::string item = t.name + (t.ref == s.original ? " (level)" : "");
                if (ImGui::Selectable(item.c_str(), t.ref == s.current)) {
                    std::string choice = t.ref == s.original ? "" : t.name;
                    if (SetMusicTrack(s.path, choice)) {
                        if (choice.empty())
                            L.music.erase(s.path);
                        else
                            L.music[s.path] = choice;
                        L.dirty = true;
                        if (L.baseScene.empty())
                            L.baseScene = CurrentScene();
                    }
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s music of %s\nplays at once (the level's track is replaced while it streams)",
                              label, s.zone.c_str());
        ImGui::PopID();
    }
}

void ApplyMusic()
{
    for (auto& [path, track] : CurrentLevel().music) {
        std::string err;
        if (!SetMusicTrack(path, track, &err))
            Log("music: %s: %s", path.c_str(), err.c_str());
    }
}

void RevertMusic()
{
    for (const MusicSlot& s : MusicSlots())  // the level's own again
        if (s.current != s.original)
            SetSegmentSound(s, s.original);
    g_choice.clear();
}

void InstallMusic()
{
    OnScenePushed([](const std::string&) {
        g_choice.clear();
        g_tracks.clear();
        g_tracksFor.clear();
        g_nextScan = 0;
    });
}

} // namespace h2::editor
