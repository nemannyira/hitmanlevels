// The site's upload check (docs/site_api.md "Checks on upload"): the client's own package reader
// (shared/levelfmt/package.cpp OpenPackage: the layout, the names, the sizes, the author's signature, the
// hashes, the game's strict level check) built to WebAssembly (build.ps1) for the Worker (src/check.js).
//   check_alloc(n) -> where the Worker writes the package's bytes
//   check_package(bytes, n) -> a JSON result: {"ok":true,"manifest":{...},"readme","briefing","objectives",
//                              "preview" (base64)} or {"ok":false,"error":"..."}
//   check_result_size() -> its length
#include <cstdint>
#include <cstdlib>
#include <string>

#include "json.hpp"
#include "levelfmt/h2_game.h"
#include "levelfmt/package.h"

namespace {
std::string g_result;

std::string Base64(const std::string& in)
{
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = uint32_t(uint8_t(in[i])) << 16 | uint32_t(uint8_t(in[i + 1])) << 8 | uint8_t(in[i + 2]);
        out += {t[v >> 18], t[(v >> 12) & 63], t[(v >> 6) & 63], t[v & 63]};
    }
    if (i + 1 == in.size()) {
        uint32_t v = uint32_t(uint8_t(in[i])) << 16;
        out += {t[v >> 18], t[(v >> 12) & 63], '=', '='};
    } else if (i + 2 == in.size()) {
        uint32_t v = uint32_t(uint8_t(in[i])) << 16 | uint32_t(uint8_t(in[i + 1])) << 8;
        out += {t[v >> 18], t[(v >> 12) & 63], t[(v >> 6) & 63], '='};
    }
    return out;
}
}

extern "C" {

__attribute__((export_name("check_alloc"))) void* check_alloc(size_t n) { return std::malloc(n ? n : 1); }

__attribute__((export_name("check_free"))) void check_free(void* p) { std::free(p); }

__attribute__((export_name("check_package"))) const char* check_package(const uint8_t* bytes, size_t n)
{
    using nlohmann::json;
    static bool registered = false;
    if (!registered) {
        h2::levelfmt::RegisterH2Game();
        registered = true;
    }
    h2::levelfmt::OpenedPackage opened;
    std::string error;
    json out;
    if (h2::levelfmt::OpenPackage(std::string(reinterpret_cast<const char*>(bytes), n), opened, &error)) {
        const h2::levelfmt::Manifest& m = opened.manifest;
        out = {{"ok", true},
               {"manifest", {{"game", m.game}, {"id", m.id}, {"version", m.version}, {"title", m.title},
                             {"base_scene", m.baseScene}, {"license", m.license},
                             {"author", {{"key", m.authorKey}, {"name", m.authorName}}}}},
               {"files", json::array()}};
        for (const h2::levelfmt::PackageFile& f : opened.files)
            out["files"].push_back(f.name);
        // what the level's page shows: the README, the laptop's briefing and objectives, the picture (all of
        // them checked by OpenPackage: plain text, the level's strict check, a clean PNG)
        if (const h2::levelfmt::PackageFile* f = opened.Find("README.md"))
            out["readme"] = f->data;
        if (const h2::levelfmt::PackageFile* f = opened.Find("preview.png"))
            out["preview"] = Base64(f->data);
        if (const h2::levelfmt::PackageFile* f = opened.Find("level.json")) {
            json level = json::parse(f->data, nullptr, false);
            const json mission = level.is_object() ? level.value("mission", json::object()) : json::object();
            if (mission.is_object() && mission.value("briefing", json()).is_string())
                out["briefing"] = mission["briefing"];
            out["objectives"] = json::array();
            if (mission.is_object() && mission.value("objectives", json()).is_array())
                for (const json& o : mission["objectives"])
                    if (o.is_object() && !o.value("hidden", false) && o.value("text", json()).is_string() &&
                        !o["text"].get<std::string>().empty())
                        out["objectives"].push_back(o["text"]);
        }
    } else {
        out = {{"ok", false}, {"error", error}};
    }
    g_result = out.dump();
    return g_result.c_str();
}

__attribute__((export_name("check_result_size"))) size_t check_result_size() { return g_result.size(); }

}
