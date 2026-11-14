#include "stats.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>

#include "crypto.h"
#include "files.h"
#include "hub.h"
#include "json.hpp"
#include "names.h"
#include "package.h"

namespace h2::levelfmt {

using nlohmann::json;
using namespace files;

namespace {

constexpr size_t kMaxRunBytes = 256u << 10;

std::vector<std::string> List(const std::string& pattern, bool folders)
{
    std::vector<std::string> names;
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(GamePath(pattern).c_str(), &d);
    if (h == INVALID_HANDLE_VALUE)
        return names;
    do {
        bool folder = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        std::string name = Utf8(d.cFileName);
        if (folder == folders && name != "." && name != "..")
            names.push_back(name);
    } while (FindNextFileW(h, &d));
    FindClose(h);
    std::sort(names.begin(), names.end());
    return names;
}

// the installed level as levelpack put it: its package.json's files unchanged, its version, the package's hash
bool Installed(const std::string& folder, const std::string& id, int& version, std::string& sha256, std::string& why)
{
    json known = ReadJsonW(DataDir() + L"known.json");
    const json* level = known.is_object() && known.value("levels", json()).is_object() && known["levels"].contains(id)
                            ? &known["levels"][id]
                            : nullptr;
    if (!level || !level->is_object() || !level->value("sha256", json()).is_string())
        return Fail(&why, "not installed by levelpack (or before it noted packages): install it again to share its plays");
    sha256 = (*level)["sha256"].get<std::string>();
    std::string text;
    json m;
    if (ReadFileA(folder + "\\package.json", text, kMaxManifestBytes))
        m = json::parse(text, nullptr, false);
    if (!m.is_object() || !m.value("version", json()).is_number_integer() || !m.value("files", json()).is_array())
        return Fail(&why, "its package.json is missing");
    version = m["version"].get<int>();
    for (const json& f : m["files"]) {
        if (!f.is_object() || !f.value("name", json()).is_string() || !f.value("sha256", json()).is_string())
            return Fail(&why, "its package.json is not a package's");
        std::string name = f["name"].get<std::string>(), data;
        if (name.rfind("scripts/", 0) == 0)
            name = name.substr(8);
        if (!ReadFileA(folder + "\\" + name, data, kMaxPackageBytes))
            return Fail(&why, name + " is missing: changed since it was installed");
        crypto::Sha256Digest d = crypto::Sha256(data.data(), data.size());
        if (crypto::Hex(d.data(), d.size()) != f["sha256"].get<std::string>())
            return Fail(&why, name + " changed since it was installed");
    }
    return true;
}

bool MoveTo(const std::string& runs, const std::string& file, const char* where)
{
    std::string dir = runs + "\\" + where;
    CreateDirectoryW(GamePath(dir).c_str(), nullptr);
    return MoveFileExW(GamePath(runs + "\\" + file).c_str(), GamePath(dir + "\\" + file).c_str(),
                       MOVEFILE_REPLACE_EXISTING) != 0;
}

} // namespace

RunsReport SendRuns(const std::string& source, const std::string& gameDir, const GameInfo& game, bool send)
{
    RunsReport r;
    std::string online = gameDir + "\\" + game.onlineDir;
    bool reachable = true;
    for (const std::string& id : List(online + "\\*", true)) {
        if (!ValidOnlineId(id))
            continue;
        std::string folder = online + "\\" + id, runs = folder + "\\runs";
        r.sentBefore += int(List(runs + "\\sent\\*.json", false).size());
        std::vector<std::string> files = List(runs + "\\*.json", false);
        if (files.empty())
            continue;
        int version = 0;
        std::string sha256, why;
        bool intact = Installed(folder, id, version, sha256, why);
        for (const std::string& file : files) {
            std::string text, note = id + "\\runs\\" + file + ": ";
            json run;
            if (ReadFileA(runs + "\\" + file, text, kMaxRunBytes))
                run = json::parse(text, nullptr, false);
            if (!run.is_object() || run.value("level", json()) != id || !run.value("run", json()).is_string()) {
                ++r.kept;
                r.notes.push_back(note + "not a run of this level");
                continue;
            }
            if (!run.value("aids", json::array()).empty() || !run.value("dev", json::array()).empty()) {
                ++r.kept;
                r.notes.push_back(note + "played with testing aids or dev settings: kept on this PC");
                continue;
            }
            if (!intact || run.value("version", json()) != version) {
                ++r.kept;
                r.notes.push_back(note + (intact ? "another version than the installed one" : why));
                continue;
            }
            if (!send || !reachable) {
                ++r.waiting;
                continue;
            }
            run["game"] = game.id;
            run["package_sha256"] = sha256;
            int status = 0;
            std::string answer, error;
            if (!Post(source, kRunsPath, run.dump(), status, answer, &error))
                status = 0;
            if (status == 200 || status == 201 || status == 409) {  // (409: that run id is there already)
                MoveTo(runs, file, "sent");
                ++r.sent;
            } else if (status == 400 || status == 422) {
                MoveTo(runs, file, "refused");
                ++r.refused;
                json a = json::parse(answer, nullptr, false);
                r.notes.push_back(note + "refused by the site: " +
                                  (a.is_object() && a.value("error", json()).is_string()
                                       ? a["error"].get<std::string>().substr(0, 200)
                                       : std::to_string(status)));
            } else {  // (no answer, not there yet, too many: the rest waits for the next time)
                ++r.waiting;
                reachable = false;
                r.notes.push_back(note + "not sent: " + (status ? "the site answered " + std::to_string(status) : error));
            }
        }
    }
    return r;
}

} // namespace h2::levelfmt
