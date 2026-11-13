// The site's upload check (docs/site_api.md "Checks on upload"): the client's own package reader
// (shared/levelfmt/package.cpp OpenPackage: the layout, the names, the sizes, the author's signature, the
// hashes, the game's strict level check) built to WebAssembly (build.ps1) for the Worker (src/check.js).
//   check_alloc(n) -> where the Worker writes the package's bytes
//   check_package(bytes, n) -> a JSON result: {"ok":true,"manifest":{...}} or {"ok":false,"error":"..."}
//   check_result_size() -> its length
#include <cstdint>
#include <cstdlib>
#include <string>

#include "json.hpp"
#include "levelfmt/h2_game.h"
#include "levelfmt/package.h"

namespace {
std::string g_result;
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
    } else {
        out = {{"ok", false}, {"error", error}};
    }
    g_result = out.dump();
    return g_result.c_str();
}

__attribute__((export_name("check_result_size"))) size_t check_result_size() { return g_result.size(); }

}
