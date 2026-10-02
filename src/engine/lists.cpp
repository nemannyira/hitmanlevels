#include "lists.h"

#include <windows.h>
#include <cstring>

#include "../log.h"
#include "scene_data.h"

namespace h2::engine {

namespace {

constexpr uintptr_t kAlloc = 0x530490;             // engine allocation (cdecl size)
constexpr uintptr_t kDblkVtable = 0x65D81C;
constexpr uintptr_t kRefContainerVtable = 0x639510;
constexpr size_t kListContainer = 0x18;
constexpr int kContainer_Clear = 0x0C / 4;
constexpr int kGeom_ReadData = 0x1B8 / 4;          // (DBLK*)

struct RefContainer {
    uintptr_t vtable;
    uint32_t* first;  // chunks: {prev, next, used dwords, ?, data...}
    uint32_t* last;
    int chunkCapacity;
    int chunkDwords;
    int count;
    int stride;       // dwords per element
};

struct Dblk {
    uintptr_t vtable;
    uint32_t size;  // block size - 4
    uint8_t* cur;
    uint8_t* start;
};

template <typename T>
T VFunc(void* obj, int index)
{
    return reinterpret_cast<T>((*reinterpret_cast<void***>(obj))[index]);
}

RefContainer* ContainerOf(ZGEOM* list)
{
    if (!list || !IsReadable(list, kListContainer + 4))
        return nullptr;
    auto c = *reinterpret_cast<RefContainer**>(reinterpret_cast<char*>(list) + kListContainer);
    if (!c || !IsReadable(c, sizeof(RefContainer)) || c->vtable != kRefContainerVtable || c->stride < 1 ||
        c->stride > 64)
        return nullptr;
    return c;
}

// [start, end) of the first reference array (tag 9) of a data block
bool FindArray(const std::vector<uint8_t>& b, size_t& start, size_t& end)
{
    size_t i = 4;
    while (i < b.size() && b[i] != 0xFF) {
        uint8_t tag = b[i] & 0x3F;
        uint32_t n = 0;
        if (tag == 9 || tag == 7 || tag == 0xC) {
            if (i + 5 > b.size())
                return false;
            memcpy(&n, &b[i + 1], 4);
            if (tag == 9) {
                start = i;
                end = i + 1 + n;
                return end <= b.size() && n >= 4;
            }
            i += 1 + n;
        } else if (tag == 1) {
            i += 9;
        } else if (tag == 2 || tag == 3 || tag == 8 || tag == 10 || tag == 11) {
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
    return false;
}

std::vector<uint8_t> AuthoredBlock(ZGEOM* list)
{
    int record = RecordOf(list);
    const auto& records = SceneRecords();
    if (record < 0 || size_t(record) >= records.size() || !records[record].data)
        return {};
    return RemappedBlock(records[record].data);
}

DWORD g_fault;
int Filter(EXCEPTION_POINTERS* ep)
{
    g_fault = ep->ExceptionRecord->ExceptionCode;
    return EXCEPTION_EXECUTE_HANDLER;
}

bool Reload(ZGEOM* list, RefContainer* c, uint8_t* block)
{
    __try {
        VFunc<void(__thiscall*)(RefContainer*)>(c, kContainer_Clear)(c);
        Dblk d;
        d.vtable = kDblkVtable;
        d.size = (*reinterpret_cast<uint32_t*>(block) & 0xFFFFFF) - 4;
        d.cur = d.start = block + 4;
        VFunc<void(__thiscall*)(ZGEOM*, Dblk*)>(list, kGeom_ReadData)(list, &d);
        return true;
    } __except (Filter(GetExceptionInformation())) {
        return false;
    }
}

} // namespace

bool IsEditableList(ZGEOM* geom)
{
    if (!geom || !ContainerOf(geom))
        return false;
    std::string c = ClassName(geom);
    return c == "ZLIST" || c == "ZConditionList";
}

std::vector<uint32_t> ListRefs(ZGEOM* list)
{
    std::vector<uint32_t> out;
    RefContainer* c = ContainerOf(list);
    if (!c)
        return out;
    int chunks = 0;
    for (uint32_t* chunk = c->first; chunk && chunks < 4096; chunk = reinterpret_cast<uint32_t*>(chunk[1]), ++chunks) {
        if (!IsReadable(chunk, 16))
            break;
        uint32_t used = chunk[2];
        if (!IsReadable(chunk + 4, used * 4))
            break;
        for (uint32_t j = 0; j < used; j += c->stride)
            out.push_back(chunk[4 + j]);
    }
    return out;
}

std::vector<uint32_t> AuthoredListRefs(ZGEOM* list)
{
    std::vector<uint32_t> out;
    std::vector<uint8_t> b = AuthoredBlock(list);
    size_t start, end;
    if (b.empty() || !FindArray(b, start, end))
        return out;
    for (size_t p = start + 5; p + 4 <= end; p += 4) {
        uint32_t v;
        memcpy(&v, &b[p], 4);
        out.push_back(v);
    }
    return out;
}

bool SetListRefs(ZGEOM* list, const std::vector<uint32_t>& refs, std::string* error)
{
    auto fail = [&](const char* why) {
        if (error)
            *error = why;
        return false;
    };
    RefContainer* c = ContainerOf(list);
    if (!c || !IsEditableList(list))
        return fail("not a list the editor can change");
    std::vector<uint8_t> b = AuthoredBlock(list);
    size_t start, end;
    if (b.empty() || !FindArray(b, start, end))
        return fail("the list has no data in the scene file");
    std::vector<uint8_t> out(b.begin(), b.begin() + start);
    out.push_back(b[start]);  // the array tag
    uint32_t n = uint32_t(4 + refs.size() * 4);
    out.insert(out.end(), reinterpret_cast<uint8_t*>(&n), reinterpret_cast<uint8_t*>(&n) + 4);
    for (uint32_t r : refs)
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(&r), reinterpret_cast<const uint8_t*>(&r) + 4);
    out.insert(out.end(), b.begin() + end, b.end());
    uint32_t header;
    memcpy(&header, out.data(), 4);
    header = (header & 0xFF000000u) | uint32_t(out.size());
    memcpy(out.data(), &header, 4);
    // the game's memory: kept (the list may point into it)
    auto block = static_cast<uint8_t*>(reinterpret_cast<void*(__cdecl*)(size_t)>(kAlloc)(out.size() + 4));
    if (!block)
        return fail("out of memory");
    memcpy(block, out.data(), out.size());
    block[out.size()] = 0xFF;
    if (!Reload(list, c, block)) {
        Log("list %s: reading the new elements failed (exception %08lX)", SafeString(list->name).c_str(), g_fault);
        return fail("the game failed to read the new elements");
    }
    return true;
}

} // namespace h2::engine
