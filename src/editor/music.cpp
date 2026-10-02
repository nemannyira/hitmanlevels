#include "music.h"

#include <windows.h>
#include <algorithm>
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

HANDLE g_streamsFile = INVALID_HANDLE_VALUE;  // the game's handle of streams.wav

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

// <scene>.WHD: entries "name\0" + {6, name offset, flags, rate, bits, ?, Ogg length, channels, data offset, PCM size, ...}
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
        g_streams[Lower(name)] = {U32(&d[fields + 32]), U32(&d[fields + 24])};  // data offset, Ogg length
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

// The tracks are Ogg Vorbis streams whose headers (identification, comment
// and code books) are the same byte for byte, and the game decodes with its
// own loop (libogg + libvorbis): headers once when the stream starts, then
// only pages of that stream's serial number. So the chosen track's audio pages
// are served rewritten as the continuation of the stream the game is reading:
// its serial number, the next page numbers, growing granule positions, no
// end-of-stream mark (it loops), new CRC. When the game starts the track
// from its beginning the chosen one's headers come first.
struct PageInfo {
    uint32_t offset;   // in the track's data
    uint32_t size;
    int64_t granule;
    uint8_t flags;
    uint32_t seq;
};

struct StreamTable {
    uint32_t serial = 0;
    std::vector<PageInfo> pages;
    uint32_t headerPages = 0, headerBytes = 0;
    std::vector<uint32_t> audioStart;  // audio page j: its offset from the first audio page
    uint32_t audioBytes = 0;
    int64_t lastGranule = 0;
};
std::map<uint32_t, StreamTable> g_tables;  // by the track's offset in streams.wav
HANDLE g_ourFile = INVALID_HANDLE_VALUE;    // our own handle (the game's position stays the game's)

struct Remap {
    Range from, to;
    bool started = false;  // a session: the chosen track is being served
    uint32_t base = 0;     // the game's position where the session started
    bool headers = false;  // the session started at the track's beginning (headers first)
    uint32_t serial = 0, seq0 = 0;
    int64_t granBase = 0;
};
std::mutex g_remapMutex;
std::vector<Remap> g_remaps;   // read by the game's streaming thread

uint32_t g_crcTable[256];

void InitCrc()
{
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t r = i << 24;
        for (int k = 0; k < 8; ++k)
            r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : r << 1;
        g_crcTable[i] = r;
    }
}

uint32_t OggCrc(const uint8_t* p, size_t n)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < n; ++i)
        crc = (crc << 8) ^ g_crcTable[((crc >> 24) & 0xFF) ^ p[i]];
    return crc;
}

bool ReadAt(uint32_t offset, void* buf, DWORD n)
{
    if (g_ourFile == INVALID_HANDLE_VALUE) {
        std::string path = GameDir() + "streams.wav";
        std::wstring w(path.begin(), path.end());
        g_ourFile = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_ourFile == INVALID_HANDLE_VALUE)
            return false;
    }
    if (SetFilePointer(g_ourFile, LONG(offset), nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
        return false;
    DWORD got = 0;
    return RawReadFile(g_ourFile, buf, n, &got) && got == n;
}

// the pages of a track (read once)
const StreamTable* Table(const Range& r)
{
    {
        std::lock_guard<std::mutex> lock(g_remapMutex);
        auto it = g_tables.find(r.start);
        if (it != g_tables.end())
            return it->second.pages.empty() ? nullptr : &it->second;
    }
    std::vector<uint8_t> data(r.length);
    StreamTable t;
    if (r.length > (64u << 20) || !ReadAt(r.start, data.data(), r.length)) {
        Log("music: track at %08X could not be read", r.start);
    } else {
        for (uint32_t p = 0; p + 27 <= r.length;) {
            const uint8_t* h = &data[p];
            if (memcmp(h, "OggS", 4) != 0)
                break;
            uint8_t nseg = h[26];
            if (p + 27 + nseg > r.length)
                break;
            uint32_t body = 0;
            for (int i = 0; i < nseg; ++i)
                body += h[27 + i];
            PageInfo pi;
            pi.offset = p;
            pi.size = 27 + nseg + body;
            memcpy(&pi.granule, h + 6, 8);
            pi.flags = h[5];
            memcpy(&pi.seq, h + 18, 4);
            if (t.pages.empty())
                memcpy(&t.serial, h + 14, 4);
            if (p + pi.size > r.length)
                break;
            t.pages.push_back(pi);
            p += pi.size;
        }
        while (t.headerPages < t.pages.size() && t.pages[t.headerPages].granule == 0)
            t.headerBytes += t.pages[t.headerPages++].size;
        for (size_t i = t.headerPages; i < t.pages.size(); ++i) {
            t.audioStart.push_back(t.audioBytes);
            t.audioBytes += t.pages[i].size;
            if (t.pages[i].granule > t.lastGranule)
                t.lastGranule = t.pages[i].granule;
        }
        if (t.audioStart.empty())
            t.pages.clear();
    }
    std::lock_guard<std::mutex> lock(g_remapMutex);
    StreamTable& stored = g_tables[r.start] = std::move(t);
    return stored.pages.empty() ? nullptr : &stored;
}

// a session starts where the game reads now
void StartSession(Remap& r, uint32_t pos, const StreamTable& from, const StreamTable& to)
{
    r.started = true;
    r.base = pos;
    uint32_t rel = pos - r.from.start;
    if (rel == 0) {  // the game starts the track: the chosen one's headers first
        r.headers = true;
        r.serial = to.serial;
        r.seq0 = 0;
        r.granBase = 0;
        return;
    }
    // in the middle: the page the game is in will not complete; ours take its number
    r.headers = false;
    r.serial = from.serial;
    r.seq0 = 0;
    r.granBase = 0;
    for (const PageInfo& p : from.pages) {
        if (p.offset + p.size > rel) {
            r.seq0 = p.seq;
            break;
        }
        if (p.granule > 0)
            r.granBase = p.granule;
    }
}

// the session's bytes from virtual offset v
DWORD Serve(const Remap& r, const StreamTable& to, uint64_t v, uint8_t* out, DWORD n)
{
    DWORD done = 0;
    std::vector<uint8_t> page;
    while (done < n) {
        uint64_t u = v + done;
        size_t index;           // the page in the chosen track
        uint64_t k;             // its number in the session
        uint64_t pageStart;     // its virtual offset
        int64_t loop = 0;
        uint32_t headerPart = r.headers ? to.headerBytes : 0;
        if (r.headers && u < to.headerBytes) {
            index = 0;
            uint64_t start = 0;
            while (start + to.pages[index].size <= u)
                start += to.pages[index++].size;
            k = index;
            pageStart = start;
        } else {
            uint64_t a = u - headerPart;
            loop = int64_t(a / to.audioBytes);
            uint32_t rem = uint32_t(a % to.audioBytes);
            size_t j = size_t(std::upper_bound(to.audioStart.begin(), to.audioStart.end(), rem) - to.audioStart.begin()) - 1;
            index = to.headerPages + j;
            k = (r.headers ? to.headerPages : 0) + uint64_t(loop) * to.audioStart.size() + j;
            pageStart = headerPart + uint64_t(loop) * to.audioBytes + to.audioStart[j];
        }
        const PageInfo& pi = to.pages[index];
        page.resize(pi.size);
        if (!ReadAt(r.to.start + pi.offset, page.data(), pi.size))
            break;
        // rewritten as the continuation of the game's stream
        uint8_t flags = pi.flags & ~uint8_t(0x06);
        if (r.headers && k == 0)
            flags |= 0x02;
        page[5] = flags;
        int64_t granule = pi.granule < 0 ? -1 : r.granBase + loop * to.lastGranule + pi.granule;
        memcpy(&page[6], &granule, 8);
        memcpy(&page[14], &r.serial, 4);
        uint32_t seq = uint32_t(r.seq0 + k);
        memcpy(&page[18], &seq, 4);
        memset(&page[22], 0, 4);
        uint32_t crc = OggCrc(page.data(), page.size());
        memcpy(&page[22], &crc, 4);
        uint64_t within = u - pageStart;
        DWORD chunk = DWORD(std::min<uint64_t>(n - done, pi.size - within));
        memcpy(out + done, page.data() + within, chunk);
        done += chunk;
    }
    return done;
}

// the game reads the original track's data: it gets the chosen track's
bool OnRead(HANDLE h, void* buf, DWORD n, DWORD* read, BOOL* result)
{
    if (h != g_streamsFile || h == INVALID_HANDLE_VALUE)
        return false;
    LONG high = 0;
    DWORD pos = SetFilePointer(h, 0, &high, FILE_CURRENT);
    if (pos == INVALID_SET_FILE_POINTER || high)
        return false;
    Remap r;
    const StreamTable* to = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_remapMutex);
        Remap* found = nullptr;
        for (Remap& x : g_remaps)
            if (pos >= x.from.start && pos < x.from.start + x.from.length)
                found = &x;
        if (!found)
            return false;
        auto a = g_tables.find(found->from.start), b = g_tables.find(found->to.start);
        if (a == g_tables.end() || b == g_tables.end() || a->second.pages.empty() || b->second.pages.empty())
            return false;
        // a new session: the choice was just made, or the game starts the track again
        if (!found->started || pos == found->from.start || pos < found->base)
            StartSession(*found, pos, a->second, b->second);
        r = *found;
        to = &b->second;
    }
    DWORD done = Serve(r, *to, uint64_t(pos - r.base), static_cast<uint8_t*>(buf), n);
    if (g_config.logFiles)
        Log("music: read @%08lX: %lu bytes of the chosen track (session at %08X)", pos, done, r.base);
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
    std::vector<Remap> old, remaps;
    {
        std::lock_guard<std::mutex> lock(g_remapMutex);
        old = g_remaps;
    }
    auto previous = [&](const Range& from) -> const Remap* {
        for (const Remap& r : old)
            if (r.from.start == from.start)
                return &r;
        return nullptr;
    };
    for (const MusicSlot& s : MusicSlots()) {
        auto from = g_streams.find(Lower(TrackName(s.original)));
        if (from == g_streams.end())
            continue;
        auto it = g_choice.find(s.path);
        const Remap* before = previous(from->second);
        Remap r;
        r.from = from->second;
        if (it == g_choice.end() || it->second.empty()) {
            if (!before)
                continue;  // never changed: the game reads it as it is
            r.to = from->second;  // back to the level's track: it too starts from its own beginning
        } else {
            auto to = g_streams.find(Lower(it->second));
            if (to == g_streams.end()) {
                Log("music: %s: stream of %s not found", s.path.c_str(), it->second.c_str());
                continue;
            }
            r.to = to->second;
        }
        if (before && before->to.start == r.to.start)  // unchanged: the session goes on
            r = *before;
        if (!Table(r.from) || !Table(r.to))
            continue;  // not Ogg pages: left as it is
        remaps.push_back(r);
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
    BuildRemaps();  // heard at the next read of that music (a few seconds at most)
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
    InitCrc();
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
