#include "script_files.h"

#include <windows.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

#include "../hook.h"
#include "../log.h"
#include "zip.h"

namespace h2::engine {

namespace {

constexpr uintptr_t kSysFile_Exists = 0x40E2C0;  // ZSysFile::(path, bool diskOnly) -> bool
constexpr uintptr_t kSysFile_Size = 0x40E1B0;    // ZSysFile::(path, bool diskOnly) -> size or -1
constexpr uintptr_t kSysFile_Read = 0x40E540;    // ZSysFile::(path, buf, size, offset, bool diskOnly) -> bytes or -1

std::mutex g_mutex;
std::map<std::string, std::vector<uint8_t>> g_files;  // canonical path -> .ppo
std::set<std::string> g_provided;                      // levels whose scripts are in g_files
int g_served;

uint32_t U32(const std::vector<uint8_t>& b, size_t at)
{
    uint32_t v = 0;
    if (at + 4 <= b.size())
        memcpy(&v, &b[at], 4);
    return v;
}

// "Scripts/C7\C7_1\..\..\AllLevels\Std2\Being2.ppo" -> "scripts\alllevels\std2\being2.ppo"
// (from "scripts\" on: the engine may put its folder in front)
std::string Canonical(const char* path)
{
    std::string s = path ? path : "";
    for (char& c : s)
        c = c == '/' ? '\\' : char(tolower(static_cast<unsigned char>(c)));
    size_t at = s.find("scripts\\");
    if (at == std::string::npos)
        return "";
    s = s.substr(at);
    std::vector<std::string> parts;
    for (size_t i = 0; i <= s.size();) {
        size_t j = s.find('\\', i);
        if (j == std::string::npos)
            j = s.size();
        std::string p = s.substr(i, j - i);
        if (p == "..") {
            if (!parts.empty())
                parts.pop_back();
        } else if (!p.empty() && p != ".") {
            parts.push_back(p);
        }
        i = j + 1;
    }
    std::string out;
    for (const std::string& p : parts)
        out += (out.empty() ? "" : "\\") + p;
    return out;
}

// the ZGF chunk tree (tools/zgf.py)
size_t NameOrData(const std::vector<uint8_t>& b, size_t c)
{
    return (U32(b, c + 4) & 0xC0000000) ? c + U32(b, c + 8) : c + 8;
}

size_t FirstChild(const std::vector<uint8_t>& b, size_t c)
{
    uint32_t f = U32(b, c + 4);
    if (!(f & 0x80000000))
        return 0;
    return (f & 0x40000000) ? c + 0x14 + size_t(U32(b, c + 0x10)) * 4 : c + 0x10;
}

bool ReadZgf(const std::string& level, std::vector<uint8_t>& zgf)
{
    std::vector<uint8_t> packed;
    std::string err;
    if (!ReadZipEntry(GameDir() + "Scenes\\" + level + "\\" + level + "__MAIN.ZIP",
                      "SCENES/" + level + "/" + level + "__MAIN.ZGF", packed, &err) ||
        packed.size() < 9) {
        Log("scripts: %s: no ZGF (%s)", level.c_str(), err.c_str());
        return false;
    }
    uint32_t size = U32(packed, 0);
    return size < (64u << 20) && Inflate(packed.data() + 9, packed.size() - 9, zgf, size);
}

// the files of a ZGF: path -> data
std::vector<std::pair<std::string, std::vector<uint8_t>>> ZgfFiles(const std::vector<uint8_t>& b)
{
    std::vector<std::pair<std::string, std::vector<uint8_t>>> out;
    size_t c = FirstChild(b, 0);
    for (uint32_t k = 0, n = U32(b, 0xC); c && k < n && c + 16 <= b.size(); ++k) {
        uint32_t size = U32(b, c + 4) & 0x3FFFFFFF;
        if (size < 12 || c + size > b.size())
            break;
        size_t name = NameOrData(b, c);
        if (name >= b.size())
            break;
        std::string path(reinterpret_cast<const char*>(&b[name]), strnlen(reinterpret_cast<const char*>(&b[name]), b.size() - name));
        if (size_t d = FirstChild(b, c); d && d + 12 <= c + size) {
            uint32_t dsize = U32(b, d + 4) & 0x3FFFFFFF;
            size_t at = NameOrData(b, d);
            if (dsize && d + dsize <= c + size && at <= d + dsize)
                out.push_back({path, std::vector<uint8_t>(b.begin() + at, b.begin() + d + dsize)});
        }
        c += size;
    }
    return out;
}

const std::vector<uint8_t>* Find(const char* path)
{
    std::string key = Canonical(path);
    if (key.size() < 4)
        return nullptr;
    if (key.compare(key.size() - 4, 4, ".sdl") == 0)
        key.replace(key.size() - 4, 4, ".ppo");
    auto it = g_files.find(key);
    return it == g_files.end() ? nullptr : &it->second;
}

void Served(const char* path)
{
    if (g_served++ < 40)
        Log("scripts: %s from another level's scripts", path);
}

using Exists_t = uint32_t(__fastcall*)(void* self, void* edx, const char* path, char diskOnly);
using Size_t = uint32_t(__fastcall*)(void* self, void* edx, const char* path, char diskOnly);
using Read_t = int(__fastcall*)(void* self, void* edx, const char* path, void* buf, uint32_t size, uint32_t offset,
                                char diskOnly);
Exists_t oExists;
Size_t oSize;
Read_t oRead;

uint32_t __fastcall hkExists(void* self, void* edx, const char* path, char diskOnly)
{
    if (oExists(self, edx, path, diskOnly))
        return true;
    std::lock_guard<std::mutex> lock(g_mutex);
    return !g_files.empty() && Find(path) != nullptr ? 1 : 0;
}

uint32_t __fastcall hkSize(void* self, void* edx, const char* path, char diskOnly)
{
    uint32_t n = oSize(self, edx, path, diskOnly);
    if (n != 0xFFFFFFFF)
        return n;
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::vector<uint8_t>* f = g_files.empty() ? nullptr : Find(path);
    return f ? uint32_t(f->size()) : n;
}

int __fastcall hkRead(void* self, void* edx, const char* path, void* buf, uint32_t size, uint32_t offset, char diskOnly)
{
    int n = oRead(self, edx, path, buf, size, offset, diskOnly);
    if (n != -1)
        return n;
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::vector<uint8_t>* f = g_files.empty() ? nullptr : Find(path);
    if (!f || offset > f->size() || !buf)
        return n;
    uint32_t count = std::min<uint32_t>(size, uint32_t(f->size() - offset));
    memcpy(buf, f->data() + offset, count);
    Served(path);
    return int(count);
}

// a level's scene file (unpacked GMS) and its strings (BUF)
bool ReadScene(const std::string& level, std::vector<uint8_t>& gms, std::vector<uint8_t>& buf)
{
    std::string zip = GameDir() + "Scenes\\" + level + "\\" + level + "__MAIN.ZIP";
    std::string base = "SCENES/" + level + "/" + level + "__MAIN.";
    std::vector<uint8_t> packed;
    return ReadZipEntry(zip, base + "GMS", packed) && ReadZipEntry(zip, base + "BUF", buf) && packed.size() > 9 &&
           Inflate(packed.data() + 9, packed.size() - 9, gms, U32(packed, 0));
}

std::string Lower(std::string s)
{
    for (char& c : s)
        c = char(tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

bool ProvideLevelScripts(const std::string& level)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_provided.count(level))
            return true;
    }
    std::vector<uint8_t> zgf;
    if (!ReadZgf(level, zgf))
        return false;
    int n = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& [path, data] : ZgfFiles(zgf)) {
        std::string key = Canonical(path.c_str());
        if (key.size() > 4 && key.compare(key.size() - 4, 4, ".ppo") == 0 && !g_files.count(key)) {
            g_files[key] = std::move(data);
            ++n;
        }
    }
    g_provided.insert(level);
    Log("scripts: %d compiled scripts of %s can be loaded", n, level.c_str());
    return true;
}

const std::vector<LevelBehaviour>& LevelBehaviours(const std::string& level)
{
    static std::map<std::string, std::vector<LevelBehaviour>> cache;
    auto it = cache.find(level);
    if (it != cache.end())
        return it->second;
    std::vector<LevelBehaviour>& out = cache[level];
    std::vector<uint8_t> gms, buf;
    if (!ReadScene(level, gms, buf)) {
        Log("scripts: %s: scene file not read", level.c_str());
        return out;
    }
    std::map<std::string, size_t> index;
    for (const SceneRecord& r : ParseSceneRecords(gms, buf)) {
        if (!r.script || r.classId != 0x2003E9)
            continue;
        ScriptInfo s;
        if (!ParseScriptIn(gms, r.script, s))
            continue;
        std::string name = ScriptShortName(s.name);
        auto [at, added] = index.emplace(Lower(name), out.size());
        if (!added) {
            out[at->second].uses++;
            continue;
        }
        LevelBehaviour b;
        b.name = name;
        for (ScriptParam& p : s.params)
            if (p.type == ParamType::Ref) {
                b.refs.push_back(p.name);
                p.bits = 0;  // the other level's objects
                p.raw.clear();
            }
        s.level = level;
        b.script = std::move(s);
        b.uses = 1;
        out.push_back(std::move(b));
    }
    return out;
}

bool IsRequiredRef(const std::string& param)
{
    // never empty in any level's characters (checked offline over every scene)
    static const char* const kRequired[] = {"weapontemplate", "m_arbiter", "waypoints", "helppoints", "safepoints",
                                            "m_alarmlist", "waypointlist", "pausepoints", "sewerpoints",
                                            "followpoints", "patrolpointlist", "m_panicarbiter", "walkpositions",
                                            "walkaroundlist", "m_rpatrolpositionlist", "walkaboutpoints"};
    std::string n = Lower(param);
    return std::any_of(std::begin(kRequired), std::end(kRequired), [&](const char* r) { return n == r; });
}

std::string MissingRequiredRef(const ScriptInfo& s)
{
    for (const ScriptParam& p : s.params)
        if (p.type == ParamType::Ref && !p.bits && IsRequiredRef(p.name))
            return p.name;
    return "";
}

void InstallScriptFiles()
{
    Hook("ZSysFile::Exists", kSysFile_Exists, &hkExists, &oExists);
    Hook("ZSysFile::Size", kSysFile_Size, &hkSize, &oSize);
    Hook("ZSysFile::Read", kSysFile_Read, &hkRead, &oRead);
}

} // namespace h2::engine
