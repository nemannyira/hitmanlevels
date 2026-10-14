#include "profile_guard.h"

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../hook.h"
#include "../log.h"
#include "../scene.h"
#include "testtools.h"

namespace h2::editor {

namespace {

constexpr uintptr_t kGlobalCom = 0x6A40EC;    // CGlobalCom (g_pGlobalCom 0x6A6C7C points here)
constexpr uintptr_t kSaveProfile = 0x551420;  // writes Save\<n>.blap from the variables (thiscall, 4 args)
constexpr uintptr_t kMissionCompleted = 0x5058B0;  // the level routine's MissionCompleted (thiscall)
constexpr uint32_t kMaxVariables = 1 << 20;   // a sane capacity (the game's is 0x2000)

struct ComHeader {
    void* vtable;
    uint32_t capacity;
    uint8_t* data;
    uint32_t used;
    uint32_t extra;
};

bool g_protected;
bool g_completed;  // the level's mission was completed on this map
ComHeader g_header;
std::vector<uint8_t> g_variables;
int g_skippedWrites;

bool ReadHeader(ComHeader& header)
{
    __try {
        header = *reinterpret_cast<const ComHeader*>(kGlobalCom);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return header.data && header.capacity != 0 && header.capacity <= kMaxVariables && header.used <= header.capacity;
}

bool SafeCopy(void* to, const void* from, size_t n)
{
    __try {
        memcpy(to, from, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Snapshot(ComHeader& header, std::vector<uint8_t>& data)
{
    if (!ReadHeader(header))
        return false;
    data.resize(header.capacity);
    return SafeCopy(data.data(), header.data, header.capacity);
}

void Restore()
{
    ComHeader now{};
    std::vector<uint8_t> current;
    if (!Snapshot(now, current)) {
        Log("profile: the global variables could not be read: not put back");
        return;
    }
    if (now.vtable != g_header.vtable || now.data != g_header.data || now.capacity != g_header.capacity) {
        Log("profile: the global variables moved (capacity %u -> %u): not put back", g_header.capacity, now.capacity);
        return;
    }
    bool changed = now.used != g_header.used || current != g_variables;
    auto* h = reinterpret_cast<ComHeader*>(kGlobalCom);
    if (!SafeCopy(g_header.data, g_variables.data(), g_header.capacity) ||
        !SafeCopy(&h->used, &g_header.used, sizeof(uint32_t) * 2)) {
        Log("profile: the global variables could not be written");
        return;
    }
    Log("profile: the level's map left: global variables put back (%s, %u bytes; %d profile writes skipped)",
        changed ? "changed by the level" : "unchanged", g_header.used, g_skippedWrites);
}

using SaveProfile_t = bool(__fastcall*)(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
SaveProfile_t oSaveProfile;

bool __fastcall hkSaveProfile(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    if (g_protected) {
        ++g_skippedWrites;
        Log("profile: not written (a level of the editor is played)");
        return true;
    }
    return oSaveProfile(self, edx, a, b, c, d);
}

using MissionCompleted_t = void(__fastcall*)(void* self, void* edx);
MissionCompleted_t oMissionCompleted;

void __fastcall hkMissionCompleted(void* self, void* edx)
{
    if (g_protected)
        g_completed = true;
    std::string aids = g_protected ? AidsUsed() : "";
    if (!aids.empty()) {  // (the statistics do not see them: a Silent Assassin with god mode)
        NoteMissionEnd();
        Log("profile: mission completed with %s", aids.c_str());
    }
    oMissionCompleted(self, edx);
}

// After a level of the editor the statistics screen's Continue would go on to
// the next mission of the campaign (strNextScene) as if the map were done:
// the main menu instead. (The map loaded again, as Test does, stays.)
std::string Redirect(const std::string& from, const std::string& to)
{
    if (!g_protected || !g_completed || _stricmp(from.c_str(), to.c_str()) == 0 || _stricmp(to.c_str(), "MainMenu") == 0)
        return "";
    Log("profile: after the level the main menu, not %s", to.c_str());
    return "MainMenu";
}

void OnLeaving(const std::string&)
{
    if (!g_protected)
        return;
    Restore();
    g_protected = false;
    g_completed = false;
    g_variables.clear();
}

} // namespace

void ProtectProfile()
{
    if (g_protected)
        return;
    if (!Snapshot(g_header, g_variables)) {
        Log("profile: the global variables could not be read: the level may count in the profile");
        return;
    }
    g_protected = true;
    g_completed = false;
    g_skippedWrites = 0;
    Log("profile: kept from the level (%u bytes of global variables noted)", g_header.used);
}

bool ProfileProtected() { return g_protected; }

void InstallProfileGuard()
{
    Hook("Profile::Save", kSaveProfile, hkSaveProfile, &oSaveProfile);
    Hook("Level::MissionCompleted", kMissionCompleted, hkMissionCompleted, &oMissionCompleted);
    SetSceneRedirect(Redirect);
    OnSceneLeaving(OnLeaving);
}

} // namespace h2::editor
