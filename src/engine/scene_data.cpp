#include "scene_data.h"

#include <windows.h>
#include <cstring>
#include <unordered_map>

#include "../game.h"
#include "../hook.h"
#include "../log.h"

namespace h2::engine {

namespace {

constexpr uintptr_t kGetGeomsData = 0x41C1C0;           // ?GetGeomsData@ZEngineDataBase@@IAEXPAXI@Z
constexpr uintptr_t kCreateGeoms = 0x41CDA0;            // ?CreateGeoms@ZEngineDataBase@@QAEXPAVREFTAB@@...
constexpr uintptr_t kRemapCreatedDataBlock = 0x41CD80;  // (DBLK*, const uint* refs, uint n, const uint* msgs)
constexpr uintptr_t kGeomPtrToRef = 0x422AE0;           // ?GeomPtrToRef@ZEngineDataBase@@QAEIPAVZGEOM@@@Z
constexpr uintptr_t kGeomRefToPtr = 0x422B00;           // ?GeomRefToPtr@ZEngineDataBase@@QAEPAVZGEOM@@I@Z
constexpr size_t kMaxGms = 64u << 20;

std::vector<uint8_t> g_gms;     // unpacked GMS of the last scene
std::vector<uint8_t> g_buf;     // its BUF (names, property blocks)
std::vector<uint32_t> g_refs;   // record index - 1 -> object ref
std::vector<uint32_t> g_msgs;   // message index -> runtime message id
std::vector<SceneRecord> g_records;
std::unordered_map<uint32_t, int> g_recordOfRef;
uint32_t g_unpackedSize;        // from the packed header GetGeomsData read
bool g_inCreateGeoms;
bool g_refsCaptured;

void* Engine()
{
    auto sys = reinterpret_cast<char*>(game::SysInterface());
    return sys ? *reinterpret_cast<void**>(sys + 0x98) : nullptr;
}

uint32_t U32(const std::vector<uint8_t>& b, size_t off)
{
    uint32_t v = 0;
    if (off + 4 <= b.size())
        memcpy(&v, b.data() + off, 4);
    return v;
}

std::string BufString(uint32_t off)
{
    if (off >= g_buf.size())
        return {};
    const char* s = reinterpret_cast<const char*>(g_buf.data() + off);
    return std::string(s, strnlen(s, g_buf.size() - off));
}

void ParseRecords()
{
    g_records.clear();
    g_recordOfRef.clear();
    uint32_t list = U32(g_gms, 0);
    uint32_t count = U32(g_gms, list);
    if (!list || count > 200000 || list + 4 + size_t(count) * 8 > g_gms.size()) {
        Log("scene data: bad geom list (%u at %08X)", count, list);
        return;
    }
    std::vector<int> groups;  // open groups (record indices)
    g_records.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t rec = U32(g_gms, list + 4 + i * 8);
        for (uint32_t pop = rec >> 25; pop && !groups.empty(); --pop)
            groups.pop_back();
        size_t off = size_t(rec & 0xFFFFFF) * 4;
        if (off + 0x30 > g_gms.size())
            break;
        SceneRecord r;
        r.index = int(i);
        r.name = BufString(U32(g_gms, off + 0x00));
        uint32_t matOff = U32(g_gms, off + 0x04), posOff = U32(g_gms, off + 0x08);
        if (matOff + 36 <= g_gms.size())
            memcpy(r.mat, g_gms.data() + matOff, 36);
        if (posOff + 12 <= g_gms.size())
            memcpy(r.pos, g_gms.data() + posOff, 12);
        r.mesh = U32(g_gms, off + 0x0C);
        r.classId = U32(g_gms, off + 0x10);
        r.events = U32(g_gms, off + 0x14);
        r.flags = U32(g_gms, off + 0x18);
        r.props = U32(g_gms, off + 0x1C);
        r.data = U32(g_gms, off + 0x20);
        r.script = U32(g_gms, off + 0x28);
        r.extra = U32(g_gms, list + 4 + i * 8 + 4);
        r.parent = groups.empty() ? -1 : groups.back();
        if (r.classId & 0x100000)
            groups.push_back(int(i));
        g_records.push_back(std::move(r));
    }
    for (size_t i = 0; i < g_refs.size() && i < g_records.size(); ++i)
        if (g_refs[i])
            g_recordOfRef[g_refs[i]] = int(i);
}

// ---- hooks ----

using GetGeomsData_t = void(__fastcall*)(void* self, void* edx, void* buf, unsigned size);
GetGeomsData_t oGetGeomsData;
void __fastcall hkGetGeomsData(void* self, void* edx, void* buf, unsigned size)
{
    oGetGeomsData(self, edx, buf, size);
    g_unpackedSize = 0;
    if (buf && size >= 9)
        memcpy(&g_unpackedSize, buf, 4);  // packed GMS: u32 unpacked size, u32 packed size, u8, deflate
}

using CreateGeoms_t = void(__fastcall*)(void* self, void* edx, void* reftab, void* dynamics, const char* gms,
                                        const char* buf, void* reftab2);
CreateGeoms_t oCreateGeoms;
void __fastcall hkCreateGeoms(void* self, void* edx, void* reftab, void* dynamics, const char* gms, const char* buf,
                              void* reftab2)
{
    g_gms.clear();
    g_buf.clear();
    g_refs.clear();
    g_msgs.clear();
    g_records.clear();
    g_recordOfRef.clear();
    if (gms && g_unpackedSize > 0x30 && g_unpackedSize < kMaxGms && IsReadable(gms, g_unpackedSize))
        g_gms.assign(gms, gms + g_unpackedSize);  // before the engine remaps blocks in place
    else
        Log("scene data: GMS not kept (size %u)", g_unpackedSize);
    // BUF is an engine allocation: its size is in the block header
    uint32_t bufSize = 0;
    if (buf && IsReadable(buf - 8, 4)) {
        memcpy(&bufSize, buf - 8, 4);
        bufSize &= 0x1FFFFFFF;
    }
    if (buf && bufSize && bufSize < kMaxGms && IsReadable(buf, bufSize))
        g_buf.assign(buf, buf + bufSize);
    g_inCreateGeoms = true;
    g_refsCaptured = false;
    oCreateGeoms(self, edx, reftab, dynamics, gms, buf, reftab2);
    g_inCreateGeoms = false;
    ParseRecords();
    Log("scene data: %zu records, GMS %zu bytes, BUF %zu bytes, %zu refs, %zu messages", g_records.size(),
        g_gms.size(), g_buf.size(), g_refs.size(), g_msgs.size());
}

using Remap_t = void(__fastcall*)(void* self, void* edx, void* dblk, const uint32_t* refs, uint32_t n,
                                  const uint32_t* msgs);
Remap_t oRemap;
void __fastcall hkRemap(void* self, void* edx, void* dblk, const uint32_t* refs, uint32_t n, const uint32_t* msgs)
{
    // every record exists by the first remap (scripts and data blocks come last)
    if (g_inCreateGeoms && !g_refsCaptured && refs && n && n < 200000 && IsReadable(refs, n * 4)) {
        g_refsCaptured = true;
        g_refs.assign(refs, refs + n);
        uint32_t names = U32(g_gms, 4);
        uint32_t msgCount = names ? U32(g_gms, names) : 0;
        if (msgs && msgCount < 10000 && IsReadable(msgs, (msgCount + 1) * 4))
            g_msgs.assign(msgs, msgs + msgCount + 1);  // 1-based
    }
    oRemap(self, edx, dblk, refs, n, msgs);
}

// Walks a typed block (as ZEngineDataBase::RemapCreatedDataBlock / 0x5BE7C0)
// and remaps references in place. `msgs` is not used in nested blocks.
void RemapValues(uint8_t* p, uint8_t* end, bool withMsgs)
{
    auto ref = [](uint32_t v) -> uint32_t {
        uint32_t idx = v & 0x7FFFFFFF;
        if (!idx)
            return v;
        return idx <= g_refs.size() ? g_refs[idx - 1] : 0;
    };
    while (p < end && *p != 0xFF) {
        uint8_t tag = *p & 0x3F;
        size_t len = 1;
        switch (tag) {
        case 1: len = 9; break;
        case 2: case 3: case 8: case 10: case 11: len = 5; break;
        case 4: case 5: len = 1 + strnlen(reinterpret_cast<char*>(p + 1), end - p - 1) + 1; break;
        case 7: case 9: case 0xC: {
            uint32_t n;
            memcpy(&n, p + 1, 4);
            len = 1 + size_t(n);
            break;
        }
        default: len = 1; break;
        }
        if (p + len > end)
            return;
        if (tag == 8) {
            uint32_t v;
            memcpy(&v, p + 1, 4);
            v = ref(v);
            memcpy(p + 1, &v, 4);
        } else if (tag == 9) {
            uint32_t n;
            memcpy(&n, p + 1, 4);
            uint32_t* a = reinterpret_cast<uint32_t*>(p + 5);
            for (uint32_t k = 1; k < n / 4; ++k, ++a)
                *a = ref(*a);
        } else if (tag == 10 && withMsgs) {
            uint32_t v;
            memcpy(&v, p + 1, 4);
            if (v)
                v = v < g_msgs.size() ? g_msgs[v] : 0;
            memcpy(p + 1, &v, 4);
        } else if (tag == 0xC) {
            uint32_t n;
            memcpy(&n, p + 1, 4);
            if (n > 4)
                RemapValues(p + 5, p + 5 + ((n & 0xFFFFFF) - 4), false);
        }
        p += len;
    }
}

// Value length of a tagged value at p (0: end or unknown).
size_t ValueLength(const uint8_t* p, const uint8_t* end)
{
    if (p >= end || *p == 0xFF)
        return 0;
    switch (*p & 0x3F) {
    case 1: return 9;
    case 2: case 3: case 8: case 10: case 11: return 5;
    case 4: case 5: return 1 + strnlen(reinterpret_cast<const char*>(p + 1), end - p - 1) + 1;
    case 7: case 9: case 0xC: {
        uint32_t n;
        memcpy(&n, p + 1, 4);
        return 1 + size_t(n);
    }
    default: return 1;
    }
}

} // namespace

void InstallSceneData()
{
    Hook("ZEngineDataBase::GetGeomsData", kGetGeomsData, hkGetGeomsData, &oGetGeomsData);
    Hook("ZEngineDataBase::CreateGeoms", kCreateGeoms, hkCreateGeoms, &oCreateGeoms);
    Hook("ZEngineDataBase::RemapCreatedDataBlock", kRemapCreatedDataBlock, hkRemap, &oRemap);
}

const std::vector<SceneRecord>& SceneRecords() { return g_records; }

uint32_t RefOf(ZGEOM* geom)
{
    void* engine = Engine();
    if (!engine || !geom)
        return 0;
    return reinterpret_cast<uint32_t(__thiscall*)(void*, ZGEOM*)>(kGeomPtrToRef)(engine, geom);
}

ZGEOM* GeomOfRef(uint32_t ref)
{
    void* engine = Engine();
    if (!engine || !ref)
        return nullptr;
    return reinterpret_cast<ZGEOM*(__thiscall*)(void*, uint32_t)>(kGeomRefToPtr)(engine, ref);
}

ZGEOM* RecordGeom(int index)
{
    if (index < 0 || size_t(index) >= g_refs.size())
        return nullptr;
    return GeomOfRef(g_refs[index]);
}

int RecordOf(const ZGEOM* geom)
{
    uint32_t ref = RefOf(const_cast<ZGEOM*>(geom));
    auto it = g_recordOfRef.find(ref);
    return it == g_recordOfRef.end() ? -1 : it->second;
}

uint32_t RemapRef(uint32_t fileRef)
{
    uint32_t idx = fileRef & 0x7FFFFFFF;
    return idx && idx <= g_refs.size() ? g_refs[idx - 1] : 0;
}

std::vector<uint8_t> RemappedBlock(uint32_t offset)
{
    uint32_t header = U32(g_gms, offset);
    size_t size = header & 0xFFFFFF;
    if (!offset || size < 4 || offset + size > g_gms.size())
        return {};
    std::vector<uint8_t> b(g_gms.begin() + offset, g_gms.begin() + offset + size);
    b.push_back(0xFF);  // guard
    RemapValues(b.data() + 4, b.data() + size, true);
    b.pop_back();
    uint32_t h = header | 0x80000000;
    memcpy(b.data(), &h, 4);
    return b;
}

std::vector<uint8_t> RawBlock(uint32_t offset)
{
    uint32_t header = U32(g_gms, offset);
    size_t size = header & 0xFFFFFF;
    if (!offset || size < 4 || offset + size > g_gms.size())
        return {};
    return std::vector<uint8_t>(g_gms.begin() + offset, g_gms.begin() + offset + size);
}

std::vector<size_t> BlockRefOffsets(const std::vector<uint8_t>& b)
{
    std::vector<size_t> out;
    const uint8_t* start = b.data();
    const uint8_t* p = start + 4;
    const uint8_t* end = start + b.size();
    while (p < end) {
        size_t len = ValueLength(p, end);
        if (!len || p + len > end)
            break;
        if ((*p & 0x3F) == 8)
            out.push_back(size_t(p + 1 - start));
        p += len;
    }
    return out;
}

bool ParseScript(uint32_t offset, ScriptInfo& out)
{
    out = {};
    uint32_t header = U32(g_gms, offset);
    size_t size = header & 0xFFFFFF;
    if (!offset || size < 8 || offset + size > g_gms.size())
        return false;
    const uint8_t* p = g_gms.data() + offset + 4;
    const uint8_t* end = g_gms.data() + offset + size;
    auto str = [&](std::string& s) {
        if (p >= end)
            return false;
        ++p;  // tag
        size_t n = strnlen(reinterpret_cast<const char*>(p), end - p);
        s.assign(reinterpret_cast<const char*>(p), n);
        p += n + 1;
        return p <= end;
    };
    std::string decl;
    if (!str(out.name) || !str(decl))
        return false;
    // "LONG m_iHitpoints=20;ZGEOMREF WeaponTemplate;..." : one value per declaration
    size_t start = 0;
    while (start < decl.size()) {
        size_t semi = decl.find(';', start);
        if (semi == std::string::npos)
            semi = decl.size();
        std::string item = decl.substr(start, semi - start);
        start = semi + 1;
        size_t eq = item.find('=');
        if (eq != std::string::npos)
            item.resize(eq);
        size_t sp = item.rfind(' ');
        if (sp == std::string::npos)
            continue;
        ScriptParam prm;
        prm.typeName = item.substr(0, sp);
        prm.name = item.substr(sp + 1);
        size_t len = ValueLength(p, end);
        if (!len)
            break;
        prm.raw.assign(p, p + len);
        uint8_t tag = *p & 0x3F;
        prm.type = tag == 1 || tag == 2 || tag == 3 || tag == 4 || tag == 8 ? ParamType(tag) : ParamType::Other;
        if (tag == 2 || tag == 3 || tag == 8)
            memcpy(&prm.bits, p + 1, 4);
        else if (tag == 4)
            prm.text.assign(reinterpret_cast<const char*>(p + 1));
        p += len;
        out.params.push_back(std::move(prm));
    }
    return true;
}

std::vector<uint8_t> BuildScriptBlock(const ScriptInfo& script)
{
    std::vector<uint8_t> b(4, 0);
    auto str = [&](const std::string& s) {
        b.push_back(0x84);
        b.insert(b.end(), s.begin(), s.end());
        b.push_back(0);
    };
    str(script.name);
    std::string decl;
    for (const ScriptParam& p : script.params)
        decl += p.typeName + " " + p.name + ";";
    str(decl);
    for (const ScriptParam& p : script.params) {
        switch (p.type) {
        case ParamType::Float:
        case ParamType::Long:
        case ParamType::Ref: {
            b.push_back(uint8_t(p.type));
            const uint8_t* v = reinterpret_cast<const uint8_t*>(&p.bits);
            b.insert(b.end(), v, v + 4);
            break;
        }
        case ParamType::String:
            b.push_back(0x04);
            b.insert(b.end(), p.text.begin(), p.text.end());
            b.push_back(0);
            break;
        default:
            b.insert(b.end(), p.raw.begin(), p.raw.end());
            break;
        }
    }
    b.push_back(0x06);
    b.push_back(0xFF);
    while (b.size() % 4)
        b.push_back(0);
    uint32_t h = uint32_t(b.size()) | 0x80000000;  // values are runtime refs already
    memcpy(b.data(), &h, 4);
    return b;
}

std::string ScriptShortName(const std::string& path)
{
    size_t slash = path.find_last_of("\\/");
    std::string s = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = s.rfind('.');
    return dot == std::string::npos ? s : s.substr(0, dot);
}

} // namespace h2::engine
