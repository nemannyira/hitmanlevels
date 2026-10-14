#include "own_map.h"

#include <windows.h>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "imgui.h"

#include "../game.h"
#include "../log.h"
#include "../scene.h"
#include "level.h"

namespace h2::editor {

namespace {

std::string g_status;
char g_name[32] = "MyMap";
bool g_removeFiles;

uint16_t U16(const std::vector<uint8_t>& b, size_t o) { return uint16_t(b[o] | (b[o + 1] << 8)); }
uint32_t U32(const std::vector<uint8_t>& b, size_t o)
{
    return uint32_t(b[o]) | (uint32_t(b[o + 1]) << 8) | (uint32_t(b[o + 2]) << 16) | (uint32_t(b[o + 3]) << 24);
}
void Put16(std::vector<uint8_t>& b, uint16_t v)
{
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
}
void Put32(std::vector<uint8_t>& b, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back(uint8_t(v >> (8 * i)));
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool WriteFile(const std::string& path, const std::vector<uint8_t>& data)
{
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!f)
            return false;
    }
    return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// "SCENES/C1-1/C1-1__MAIN.GMS" -> "SCENES/MyMap/MyMap__MAIN.GMS"
std::string Rename(const std::string& entry, const std::string& from, const std::string& to)
{
    std::string s = entry;
    for (const std::string& pat : {"/" + from + "/", from + "__"}) {
        std::string rep = pat[0] == '/' ? "/" + to + "/" : to + "__";
        size_t at = 0;
        while ((at = s.find(pat, at)) != std::string::npos) {
            s.replace(at, pat.size(), rep);
            at += rep.size();
        }
    }
    return s;
}

// A copy of a ZIP with its entries renamed: each entry's data as it is (no
// recompression), a new central directory. Only plain archives (no data
// descriptors, no ZIP64), as the game's are.
bool RewriteZip(const std::vector<uint8_t>& in, const std::string& from, const std::string& to,
                std::vector<uint8_t>& out, std::string* error)
{
    auto fail = [&](const char* why) {
        if (error)
            *error = why;
        return false;
    };
    struct Entry {
        std::vector<uint8_t> header;  // the local header's fixed fields (26 bytes after the signature)
        std::string name;
        uint32_t offset;
    };
    std::vector<Entry> entries;
    size_t p = 0;
    while (p + 30 <= in.size() && U32(in, p) == 0x04034b50) {
        uint16_t flags = U16(in, p + 6);
        uint32_t csize = U32(in, p + 18);
        uint16_t nameLen = U16(in, p + 26), extraLen = U16(in, p + 28);
        if (flags & 0x8)
            return fail("the map's archive has data descriptors");
        size_t data = p + 30 + nameLen + extraLen;
        if (data + csize > in.size() || csize == 0xFFFFFFFF)
            return fail("the map's archive is cut short (or ZIP64)");
        Entry e;
        e.header.assign(in.begin() + long(p) + 4, in.begin() + long(p) + 26);  // version .. usize
        e.name = Rename(std::string(reinterpret_cast<const char*>(&in[p + 30]), nameLen), from, to);
        e.offset = uint32_t(out.size());
        Put32(out, 0x04034b50);
        out.insert(out.end(), e.header.begin(), e.header.end());
        Put16(out, uint16_t(e.name.size()));
        Put16(out, 0);  // no extra field
        out.insert(out.end(), e.name.begin(), e.name.end());
        out.insert(out.end(), in.begin() + long(data), in.begin() + long(data + csize));
        entries.push_back(std::move(e));
        p = data + csize;
        if (entries.size() > 4096)
            return fail("too many entries");
    }
    if (entries.empty())
        return fail("the map's archive has no entries");
    uint32_t dir = uint32_t(out.size());
    for (const Entry& e : entries) {
        Put32(out, 0x02014b50);
        Put16(out, 20);                                          // made by
        out.insert(out.end(), e.header.begin(), e.header.end());  // needed .. usize (22 bytes)
        Put16(out, uint16_t(e.name.size()));
        Put16(out, 0);  // extra
        Put16(out, 0);  // comment
        Put16(out, 0);  // disk
        Put16(out, 0);  // internal attributes
        Put32(out, 0);  // external attributes
        Put32(out, e.offset);
        out.insert(out.end(), e.name.begin(), e.name.end());
    }
    uint32_t dirSize = uint32_t(out.size()) - dir;
    Put32(out, 0x06054b50);
    Put16(out, 0);
    Put16(out, 0);
    Put16(out, uint16_t(entries.size()));
    Put16(out, uint16_t(entries.size()));
    Put32(out, dirSize);
    Put32(out, dir);
    Put16(out, 0);
    return true;
}

// "C1-1\C1-1__MAIN" -> "C1-1"
std::string MapFolder(const std::string& scene)
{
    size_t slash = scene.find('\\');
    return slash == std::string::npos ? scene : scene.substr(0, slash);
}

bool ValidName(const std::string& n)
{
    if (n.size() < 3 || n.size() > 24 || !isalpha(static_cast<unsigned char>(n[0])))
        return false;
    for (char c : n)
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'))
            return false;
    return true;
}

} // namespace

std::string OwnSceneName(const std::string& name) { return name + "\\" + name + "__MAIN"; }

bool MakeOwnMap(const std::string& name, std::string* error)
{
    auto fail = [&](const std::string& why) {
        if (error)
            *error = why;
        return false;
    };
    LevelDocument& L = CurrentLevel();
    if (L.folder.empty())
        return fail("save the level first");
    if (!ValidName(name))
        return fail("a name of 3 .. 24 letters, digits, _ or - (a letter first)");
    std::string original = L.ownMapOf.empty() ? L.baseScene : L.ownMapOf;
    if (original.empty())
        return fail("the level has no map yet");
    std::string from = MapFolder(original);
    std::string scenes = GameDir() + "Scenes\\";
    std::string dir = scenes + name + "\\";
    if (GetFileAttributesA(dir.c_str()) != INVALID_FILE_ATTRIBUTES)
        return fail("Scenes\\" + name + " is there already (a map of the game or another level's)");
    std::vector<uint8_t> zip, out;
    if (!ReadFile(scenes + from + "\\" + from + "__MAIN.ZIP", zip))
        return fail("the map's archive was not found: Scenes\\" + from);
    std::string err;
    if (!RewriteZip(zip, from, name, out, &err))
        return fail(err);
    if (!CreateDirectoryA(dir.c_str(), nullptr))
        return fail("Scenes\\" + name + " could not be made");
    if (!WriteFile(dir + name + "__MAIN.ZIP", out))
        return fail("the new map could not be written");
    for (const char* ext : {"__MAIN.WAV", "__MAIN.WHD", "__LOAD.WAV", "__LOAD.WHD"}) {
        std::string src = scenes + from + "\\" + from + ext;
        if (GetFileAttributesA(src.c_str()) != INVALID_FILE_ATTRIBUTES &&
            !CopyFileA(src.c_str(), (dir + name + ext).c_str(), FALSE))
            Log("own map: %s%s not copied", from.c_str(), ext);
    }
    L.ownMapOf = original;
    L.baseScene = OwnSceneName(name);
    L.dirty = true;
    if (!SaveLevel(&err))
        return fail("the map was made, the level not saved: " + err);
    Log("own map: %s made from %s (%zu bytes)", name.c_str(), from.c_str(), out.size());
    return true;
}

bool BackToOriginalMap(bool removeFiles, std::string* error)
{
    LevelDocument& L = CurrentLevel();
    if (L.ownMapOf.empty()) {
        if (error)
            *error = "the level is on a map of the game";
        return false;
    }
    std::string own = MapFolder(L.baseScene);
    L.baseScene = L.ownMapOf;
    L.ownMapOf.clear();
    L.dirty = true;
    std::string err;
    bool saved = SaveLevel(&err);
    if (removeFiles && ValidName(own) && _stricmp(CurrentScene().c_str(), OwnSceneName(own).c_str()) != 0) {
        std::string dir = GameDir() + "Scenes\\" + own + "\\";
        for (const char* ext : {"__MAIN.ZIP", "__MAIN.WAV", "__MAIN.WHD", "__LOAD.WAV", "__LOAD.WHD"})
            DeleteFileA((dir + own + ext).c_str());
        RemoveDirectoryA(dir.c_str());
    }
    if (!saved && error)
        *error = err;
    return saved;
}

void DrawOwnMap()
{
    LevelDocument& L = CurrentLevel();
    if (!ImGui::CollapsingHeader("A map of its own"))
        return;
    if (!L.ownMapOf.empty()) {
        ImGui::TextWrapped("This level has a map of its own: %s (a copy of %s).", L.baseScene.c_str(),
                           L.ownMapOf.c_str());
        bool here = _stricmp(CurrentScene().c_str(), L.baseScene.c_str()) == 0;
        if (!here && ImGui::Button("Load it"))
            RequestScene(L.baseScene);
        ImGui::Checkbox("delete its files", &g_removeFiles);
        ImGui::SameLine();
        ImGui::BeginDisabled(here && g_removeFiles);
        if (ImGui::Button("Back to the original map")) {
            std::string err;
            g_status = BackToOriginalMap(g_removeFiles, &err) ? "back on " + L.baseScene : err;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Its files can be deleted while another map is loaded");
    } else {
        ImGui::TextWrapped("The map copied under a new name: the level is played as a map of its own "
                           "(its name in the launcher, its own saved games), not as %s.",
                           L.baseScene.empty() ? "the map" : L.baseScene.c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
        ImGui::InputText("##ownname", g_name, sizeof(g_name));
        ImGui::SameLine();
        ImGui::BeginDisabled(L.folder.empty() || L.baseScene.empty());
        if (ImGui::Button("Make the map")) {
            std::string err;
            if (MakeOwnMap(g_name, &err)) {
                g_status = "made: " + L.baseScene + " (loading it)";
                RequestScene(L.baseScene);
            } else {
                g_status = err;
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(L.folder.empty() ? "Save the level first"
                                               : "Scenes\\<name>: about 15 MB (the map's archive and sounds)");
    }
    if (!g_status.empty())
        ImGui::TextWrapped("%s", g_status.c_str());
}

} // namespace h2::editor
