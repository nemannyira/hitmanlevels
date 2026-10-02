#include "music.h"

#include <windows.h>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>

#include "imgui.h"

#include "../config.h"
#include "../diag.h"
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

// ---- streams.wav: where each music track's data is (from the scene's .WHD) ----

struct Range {
    uint32_t start = 0, length = 0;
};
std::map<std::string, Range> g_streams;  // track name (lower case) -> its data in streams.wav
std::string g_streamsFor;

struct Remap {
    Range from, to;
};
std::mutex g_remapMutex;
std::vector<Remap> g_remaps;   // read by the game's streaming thread
HANDLE g_streamsFile = INVALID_HANDLE_VALUE;

std::string Lower(std::string s)
{
    for (char& c : s)
        c = char(tolower(static_cast<unsigned char>(c)));
    return s;
}

uint32_t U32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

// <scene>.WHD: entries "name\0" + {6, name offset, flags, rate, bits, ?, ?, channels, data offset, length, ...}
void ReadStreams()
{
    g_streams.clear();
    g_streamsFor = CurrentScene();
    std::ifstream f(GameDir() + "Scenes\\" + CurrentScene() + ".WHD", std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const char dir[] = "Streams\\Music\\";
    const size_t n = sizeof(dir) - 1;
    for (size_t i = 0; i + n < d.size(); ++i) {
        if (d[i] != 'S' || memcmp(&d[i], dir, n) != 0)
            continue;
        size_t len = strnlen(reinterpret_cast<const char*>(&d[i]), d.size() - i);
        size_t fields = i + len + 1;
        if (fields + 48 > d.size() || U32(&d[fields]) != 6 || U32(&d[fields + 4]) != i)
            continue;
        std::string name(reinterpret_cast<const char*>(&d[i]) + n, len - n);
        if (name.size() > 4)
            name.resize(name.size() - 4);  // ".wav"
        g_streams[Lower(name)] = {U32(&d[fields + 32]), U32(&d[fields + 36])};
        i = fields + 47;
    }
    Log("music: %d streams in %s.WHD", int(g_streams.size()), CurrentScene().c_str());
}

void OnOpen(const char* name, HANDLE h)
{
    size_t len = strlen(name);
    if (len >= 11 && _stricmp(name + len - 11, "streams.wav") == 0)
        g_streamsFile = h;
}

// the game reads the original track's data: it gets the chosen track's (looped)
bool OnRead(HANDLE h, void* buf, DWORD n, DWORD* read, BOOL* result)
{
    if (h != g_streamsFile || h == INVALID_HANDLE_VALUE)
        return false;
    LONG high = 0;
    DWORD pos = SetFilePointer(h, 0, &high, FILE_CURRENT);
    if (pos == INVALID_SET_FILE_POINTER || high)
        return false;
    Range from, to;
    {
        std::lock_guard<std::mutex> lock(g_remapMutex);
        bool found = false;
        for (const Remap& r : g_remaps) {
            if (pos >= r.from.start && pos < r.from.start + r.from.length && r.to.length) {
                from = r.from;
                to = r.to;
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    uint32_t target = to.start + (pos - from.start) % to.length;
    DWORD done = 0;
    while (done < n) {
        DWORD chunk = std::min<DWORD>(n - done, to.start + to.length - target);
        SetFilePointer(h, LONG(target), nullptr, FILE_BEGIN);
        DWORD got = 0;
        if (!RawReadFile(h, static_cast<char*>(buf) + done, chunk, &got) || !got)
            break;
        done += got;
        target = to.start;  // the chosen track loops
        if (got < chunk)
            break;
    }
    if (g_config.logFiles)
        Log("music: read @%08lX served from %08X", pos, to.start + (pos - from.start) % to.length);
    SetFilePointer(h, LONG(pos + done), nullptr, FILE_BEGIN);  // the game's position, in the original
    if (read)
        *read = done;
    *result = TRUE;
    return true;
}

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

// the redirections from the choices
void BuildRemaps()
{
    if (g_streamsFor != CurrentScene())
        ReadStreams();
    std::vector<Remap> remaps;
    for (const MusicSlot& s : MusicSlots()) {
        auto it = g_choice.find(s.path);
        if (it == g_choice.end() || it->second.empty())
            continue;
        auto from = g_streams.find(Lower(TrackName(s.original)));
        auto to = g_streams.find(Lower(it->second));
        if (from == g_streams.end() || to == g_streams.end()) {
            Log("music: %s: stream of %s or %s not found", s.path.c_str(), TrackName(s.original).c_str(),
                it->second.c_str());
            continue;
        }
        remaps.push_back({from->second, to->second});
    }
    std::lock_guard<std::mutex> lock(g_remapMutex);
    g_remaps = remaps;
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
    g_choice[slotPath] = track;
    BuildRemaps();
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
    g_choice.clear();
    std::lock_guard<std::mutex> lock(g_remapMutex);
    g_remaps.clear();
}

void InstallMusic()
{
    SetOpenWatch(OnOpen);
    SetReadFilter(OnRead);
    OnScenePushed([](const std::string&) {
        g_choice.clear();
        g_tracks.clear();
        g_tracksFor.clear();
        g_nextScan = 0;
        std::lock_guard<std::mutex> lock(g_remapMutex);
        g_remaps.clear();
    });
}

} // namespace h2::editor
