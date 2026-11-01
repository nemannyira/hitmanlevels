#include "level_check.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <set>

namespace h2::levelfmt {

using nlohmann::json;

namespace {

bool EqualNoCase(const std::string& a, const char* b)
{
    size_t n = strlen(b);
    if (a.size() != n)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// the walk of one level file: each check reports where it failed
class Checker {
public:
    Checker(Report& r, bool strict) : r_(r), strict_(strict) {}

    // a value the loader leaves out (strict: the level is refused)
    void Bad(const std::string& where, const std::string& what) { r_.issues.push_back({strict_, where, what}); }
    // refused whatever the mode (it cannot be a level)
    void Fatal(const std::string& where, const std::string& what) { r_.issues.push_back({true, where, what}); }

    // an object with only these fields
    bool Object(const json& o, const std::string& where, std::initializer_list<const char*> fields)
    {
        if (!o.is_object()) {
            Bad(where, "not an object");
            return false;
        }
        for (auto it = o.begin(); it != o.end(); ++it)
            if (std::none_of(fields.begin(), fields.end(), [&](const char* f) { return it.key() == f; }))
                Bad(where + "." + Clip(it.key()), "a field the editor does not write");
        return true;
    }

    // text without control characters (a new line only where `lines`), at most `max` bytes
    void Text(const json& o, const char* key, const std::string& where, size_t max, bool lines = false)
    {
        if (!o.contains(key))
            return;
        const json& v = o[key];
        std::string at = where + "." + key;
        if (!v.is_string()) {
            Bad(at, "not text");
            return;
        }
        const std::string& s = v.get_ref<const std::string&>();
        if (s.size() > max)
            Bad(at, "longer than " + std::to_string(max) + " bytes");
        for (unsigned char ch : s)
            if (ch < 0x20 && !(lines && (ch == '\n' || ch == '\t'))) {
                Bad(at, "a control character");
                break;
            }
    }

    // text of these characters only (names, paths of the game's files)
    void Name(const json& o, const char* key, const std::string& where, size_t max, const char* extra)
    {
        if (!o.contains(key))
            return;
        std::string at = where + "." + key;
        if (!o[key].is_string()) {
            Bad(at, "not text");
            return;
        }
        const std::string& s = o[key].get_ref<const std::string&>();
        if (s.size() > max)
            Bad(at, "longer than " + std::to_string(max) + " bytes");
        else if (s.find("..") != std::string::npos ||
                 !std::all_of(s.begin(), s.end(), [&](char ch) {
                     return std::isalnum(static_cast<unsigned char>(ch)) || strchr(extra, ch) != nullptr;
                 }))
            Bad(at, "a character a name of the game's has not");
    }

    void Bool(const json& o, const char* key, const std::string& where)
    {
        if (o.contains(key) && !o[key].is_boolean())
            Bad(where + "." + key, "not true / false");
    }

    void Int(const json& o, const char* key, const std::string& where, long long lo, long long hi)
    {
        if (!o.contains(key))
            return;
        const json& v = o[key];
        std::string at = where + "." + key;
        if (!v.is_number_integer())
            Bad(at, "not a whole number");
        else if (v.get<long long>() < lo || v.get<long long>() > hi)
            Bad(at, "out of range (" + std::to_string(lo) + ".." + std::to_string(hi) + ")");
    }

    void Number(const json& o, const char* key, const std::string& where, double lo, double hi)
    {
        if (o.contains(key))
            NumberValue(o[key], where + "." + key, lo, hi);
    }

    bool NumberValue(const json& v, const std::string& at, double lo, double hi)
    {
        if (!v.is_number()) {
            Bad(at, "not a number");
            return false;
        }
        double d = v.get<double>();
        if (!std::isfinite(d) || d < lo || d > hi) {
            Bad(at, "out of range");
            return false;
        }
        return true;
    }

    // an array of `n` numbers in [-limit, limit] (n 0: any length up to `max`)
    void Numbers(const json& o, const char* key, const std::string& where, size_t n, double limit, size_t max = 0)
    {
        if (!o.contains(key))
            return;
        const json& v = o[key];
        std::string at = where + "." + key;
        if (!v.is_array() || (n && v.size() != n) || (!n && v.size() > max)) {
            Bad(at, n ? "not " + std::to_string(n) + " numbers" : "not a list of at most " + std::to_string(max));
            return;
        }
        for (size_t i = 0; i < v.size(); ++i)
            if (!NumberValue(v[i], at + "[" + std::to_string(i) + "]", -limit, limit))
                return;
    }

    // an array of at most `max` elements, each checked by `each(element, where)`
    template <typename Fn>
    void Array(const json& o, const char* key, const std::string& where, size_t max, Fn each)
    {
        if (!o.contains(key))
            return;
        const json& v = o[key];
        std::string at = where.empty() ? key : where + "." + key;
        if (!v.is_array()) {
            Bad(at, "not a list");
            return;
        }
        if (v.size() > max)
            Bad(at, std::to_string(v.size()) + " elements, " + std::to_string(max) + " at most");
        for (size_t i = 0; i < v.size() && i < max; ++i)
            each(v[i], at + "[" + std::to_string(i) + "]");
    }

    // pos (3 numbers), matrix (3x3) and hidden, as the editor writes a place
    void Place(const json& o, const std::string& where, bool required = true)
    {
        if (required && (!o.contains("pos") || !o.contains("matrix")))
            Bad(where, "no pos / matrix");
        Numbers(o, "pos", where, 3, 1e7);  // (the engine has no use for more; NaN breaks its trees)
        Numbers(o, "matrix", where, 9, 1e3);
        Bool(o, "hidden", where);
    }

    void Params(const json& o, const std::string& where)
    {
        Array(o, "params", where, 64, [&](const json& p, const std::string& at) {
            if (!Object(p, at, {"name", "type", "value"}))
                return;
            Name(p, "name", at, 64, "_");
            Name(p, "type", at, 16, "_");
            Text(p, "value", at, 1024);
        });
    }

    static std::string Clip(const std::string& s) { return s.size() > 40 ? s.substr(0, 40) + "..." : s; }

    // an escalation's thing: "stage" (2..stages) and "gone_from" (after its stage, ..stages)
    void Span(const json& o, const std::string& where)
    {
        if (!o.contains("stage") && !o.contains("gone_from"))
            return;
        if (stages_ == 0) {
            Bad(where, "stages of a level without stages");
            return;
        }
        Int(o, "stage", where, 1, stages_);
        Int(o, "gone_from", where, 2, stages_);
        if (o.contains("stage") && o.contains("gone_from") && o["stage"].is_number_integer() &&
            o["gone_from"].is_number_integer() && o["gone_from"].get<long long>() <= o["stage"].get<long long>())
            Bad(where + ".gone_from", "not after its stage");
    }

    int stages_ = 0;  // the level's (Span)

private:
    Report& r_;
    bool strict_;
};

constexpr const char* kSceneChars = "_-\\";      // "C1-1\\C1-1__MAIN"
constexpr const char* kLevelChars = "_-";        // a game level's name: "C3-2a"

void CheckMission(Checker& c, const json& m)
{
    const std::string at = "mission";
    if (!c.Object(m, at, {"objectives", "end_when_done", "exit", "exit_radius", "base_ends", "base_files", "briefing",
                          "names", "files"}))
        return;
    c.Array(m, "objectives", at, kMaxObjectives, [&](const json& o, const std::string& w) {
        if (!c.Object(o, w, {"slot", "text", "goal", "target", "replaces_original", "radius", "place", "fails_mission",
                             "stage", "gone_from", "hidden"}))
            return;
        c.Span(o, w);
        c.Int(o, "slot", w, 1, 6);
        c.Text(o, "text", w, 256);
        if (o.contains("goal")) {
            static const std::set<std::string> goals = {"level", "kill", "reach", "take", "knockout", "deliver", "unseen"};
            if (!o["goal"].is_string() || !goals.count(o["goal"].get<std::string>()))
                c.Bad(w + ".goal", "not a goal of the editor's");
        }
        c.Text(o, "target", w, 512);
        c.Bool(o, "replaces_original", w);
        c.Bool(o, "hidden", w);
        c.Number(o, "radius", w, 50, 5000);
        c.Text(o, "place", w, 512);
        c.Bool(o, "fails_mission", w);
    });
    c.Bool(m, "end_when_done", at);
    c.Text(m, "exit", at, 512);
    c.Number(m, "exit_radius", at, 0, 100000);
    c.Bool(m, "base_ends", at);
    c.Bool(m, "base_files", at);
    c.Text(m, "briefing", at, 4096, true);
    // the laptop's files: [{name, image}] at most 5, the image a file of the level's folder (no path)
    c.Array(m, "files", at, 5, [&](const json& f, const std::string& w) {
        if (!c.Object(f, w, {"name", "image"}))
            return;
        c.Text(f, "name", w, 64);
        c.Name(f, "image", w, 64, "_-. ");
        if (f.contains("image") && f["image"].is_string()) {
            std::string img = f["image"].get<std::string>();
            std::string ext = img.size() > 4 ? img.substr(img.size() - 4) : "";
            for (char& ch : ext)
                ch = char(tolower(static_cast<unsigned char>(ch)));
            if (img.find("..") != std::string::npos || (ext != ".png" && ext != ".jpg" && ext != ".bmp"))
                c.Bad(w + ".image", "not a .png, .jpg or .bmp of the level's folder");
        }
    });
    // objects' names of the level maker's: {path: name}
    if (m.contains("names")) {
        const json& n = m["names"];
        if (!n.is_object() || n.size() > 200) {
            c.Bad(at + ".names", "not an object of at most 200 names");
        } else {
            for (auto it = n.begin(); it != n.end(); ++it) {
                if (it.key().size() > 512)
                    c.Bad(at + ".names", "a path longer than 512");
                c.Text(n, it.key().c_str(), at + ".names", 40);
            }
        }
    }
}

void CheckVehicle(Checker& c, const json& v, const std::string& w)
{
    if (!c.Object(v, w, {"object", "route", "speed", "loop", "reverse", "model", "spawned_id", "parts", "wheels", "points"}))
        return;
    if (!v.contains("object"))
        c.Bad(w, "no object");
    c.Text(v, "object", w, 512);
    c.Array(v, "route", w, 256, [&](const json& p, const std::string& at) {
        if (!p.is_string() || p.get_ref<const std::string&>().size() > 512)
            c.Bad(at, "not a path");
    });
    c.Number(v, "speed", w, 10, 20000);
    c.Bool(v, "loop", w);
    c.Bool(v, "reverse", w);
    c.Int(v, "model", w, -1, 1 << 24);
    c.Text(v, "spawned_id", w, 128);
    c.Array(v, "parts", w, 64, [&](const json& p, const std::string& at) {
        if (!c.Object(p, at, {"record", "local"}))
            return;
        c.Int(p, "record", at, 0, 1 << 24);
        c.Numbers(p, "local", at, 12, 1e6);
    });
    for (const char* key : {"wheels", "points"})
        c.Array(v, key, w, key[0] == 'w' ? 4 : 256, [&](const json& p, const std::string& at) {
            if (!p.is_array() || p.size() != 3) {
                c.Bad(at, "not 3 numbers");
                return;
            }
            for (size_t i = 0; i < 3; ++i)
                c.NumberValue(p[i], at, -1e6, 1e6);
        });
}

} // namespace

const std::vector<std::string>& MapIconNames()
{
    static const std::vector<std::string> names = {
        "Icon_Exit",        "Icon_PointOfInterest", "Icon_Pickup",       "Icon_Target",      "Icon_VIP",
        "Icon_Enemy",       "Icon_Innocent",        "Icon_Police",       "Icon_Stairs",      "Icon_StairsUP",
        "Icon_StairsDOWN",  "Icon_LadderUP",        "Icon_LadderDOWN",   "Icon_SewerHatchUP", "Icon_SewerHatchDOWN",
        "Icon_Elevator",    "Icon_Vehicle",         "Icon_Generator"};
    return names;
}

bool Report::Ok() const
{
    return std::none_of(issues.begin(), issues.end(), [](const Issue& i) { return i.error; });
}

std::string Report::Summary(size_t max) const
{
    std::string out;
    size_t n = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (const Issue& i : issues)
            if (i.error == (pass == 0) && n++ < max)
                out += std::string(i.error ? "error: " : "warning: ") + i.where + ": " + i.what + "\n";
    if (n > max)
        out += "... " + std::to_string(n - max) + " more\n";
    return out;
}

Report CheckLevel(const json& j, bool strict)
{
    Report r;
    Checker c(r, strict);
    if (!j.is_object() || j.value("format", json()) != "h2level") {
        c.Fatal("format", "not an h2level file");
        return r;
    }
    c.Object(j, "level",
             {"format", "version", "name", "base_scene", "empty_map", "blank_map", "map_models", "own_map_of",
              "start_outfit", "place_floor", "floor_object", "sky", "objects", "spawned", "npcs", "npc_edits",
              "mission", "music", "vehicles", "doors", "new_lists", "lists", "scripts", "imports", "package", "stages", "rules",
              "replay"});
    c.Int(j, "version", "level", 1, 1);
    c.Text(j, "name", "level", 64);
    c.Name(j, "base_scene", "level", 128, kSceneChars);
    // the level's package (package.h): its id and the version it was exported last as
    if (j.contains("package") && c.Object(j["package"], "level.package", {"id", "version"})) {
        const json& p = j["package"];
        if (!p.contains("id") || !p["id"].is_string() || !ValidOnlineId(p["id"].get<std::string>()))
            c.Bad("level.package.id", "not [a-z0-9]{8,32}");
        if (!p.contains("version"))
            c.Bad("level.package.version", "missing");
        c.Int(p, "version", "level.package", 1, 0x7FFFFFFF);
    }
    for (const char* key : {"empty_map", "blank_map", "map_models"})
        c.Bool(j, key, "level");
    if (j.contains("own_map_of")) {  // a map of its own: [A-Za-z][A-Za-z0-9_-]{2,23}
        const json& v = j["own_map_of"];
        if (!v.is_string() || v.get_ref<const std::string&>().size() < 3 || v.get_ref<const std::string&>().size() > 24 ||
            !std::isalpha(static_cast<unsigned char>(v.get_ref<const std::string&>()[0])))
            c.Bad("level.own_map_of", "not the name of a map of its own");
        else
            c.Name(j, "own_map_of", "level", 24, "_-");
    }
    c.Name(j, "start_outfit", "level", 64, "_-");
    for (const char* key : {"place_floor", "sky"})
        if (j.contains(key) && c.Object(j[key], std::string("level.") + key, {"level", "mesh"})) {
            c.Name(j[key], "level", std::string("level.") + key, 16, kLevelChars);
            c.Int(j[key], "mesh", std::string("level.") + key, 0, 0xFFFFFFFFLL);
        }
    c.Text(j, "floor_object", "level", 512);
    // an escalation (docs/escalation.md): its stages, its rules, replaying the stages done
    if (j.contains("stages") && j["stages"].is_array())
        c.stages_ = int(std::min<size_t>(j["stages"].size(), kMaxStages));
    c.Array(j, "stages", "", kMaxStages, [&](const json& o, const std::string& w) {
        if (!c.Object(o, w, {"name", "text"}))
            return;
        c.Text(o, "name", w, 63);
        c.Text(o, "text", w, 1023, true);
    });
    if (j.contains("stages") && j["stages"].is_array() && j["stages"].empty())
        c.Bad("level.stages", "none (a level without stages has no \"stages\")");
    c.Array(j, "rules", "", kMaxRules, [&](const json& o, const std::string& w) {
        if (!c.Object(o, w, {"kind", "stage", "gone_from", "broken", "seconds", "weapon"}))
            return;
        static const std::set<std::string> kinds = {"unseen",       "time",      "no_innocents", "no_disguise",
                                                    "one_disguise", "kill_with", "no_firearm", "no_bodies"};
        if (!o.contains("kind") || !o["kind"].is_string() || !kinds.count(o["kind"].get<std::string>()))
            c.Bad(w + ".kind", "not a rule of the editor's");
        if (o.contains("broken") && (!o["broken"].is_string() || (o["broken"] != "fail" && o["broken"] != "note")))
            c.Bad(w + ".broken", "not fail / note");
        c.Int(o, "seconds", w, 10, 4 * 3600);
        c.Name(o, "weapon", w, 64, "_-");  // (a kind of weapon or a weapon template's name)
        c.Span(o, w);
    });
    if (j.contains("rules") && !j.contains("stages"))
        c.Bad("level.rules", "rules of a level without stages");
    c.Bool(j, "replay", "level");

    c.Array(j, "objects", "", kMaxEdits, [&](const json& o, const std::string& w) {
        if (!c.Object(o, w, {"pos", "matrix", "hidden", "path", "class", "stage", "gone_from"}))
            return;
        c.Place(o, w);
        c.Span(o, w);
        if (!o.contains("path"))
            c.Bad(w, "no path");
        c.Text(o, "path", w, 512);
        c.Name(o, "class", w, 64, "_");
    });
    const auto& icons = MapIconNames();
    size_t mapIcons = 0;
    std::set<std::string> ids;
    c.Array(j, "spawned", "", kMaxSpawned, [&](const json& o, const std::string& w) {
        if (!c.Object(o, w, {"pos", "matrix", "hidden", "id", "source_scene", "source_path", "kind", "parent",
                             "record", "parts", "import", "map_icon", "stage", "gone_from"}))
            return;
        c.Place(o, w);
        c.Span(o, w);
        c.Text(o, "id", w, 64);
        if (o.contains("id") && o["id"].is_string() && !ids.insert(o["id"].get<std::string>()).second)
            c.Bad(w + ".id", "twice");
        c.Name(o, "source_scene", w, 128, kSceneChars);
        c.Text(o, "source_path", w, 512);
        if (o.contains("kind") && (!o["kind"].is_string() || (o["kind"] != "item" && o["kind"] != "point" && o["kind"] != "group")))
            c.Bad(w + ".kind", "not item / point / group");
        c.Text(o, "parent", w, 512);
        c.Int(o, "record", w, -1, 1 << 24);
        c.Array(o, "parts", w, 64, [&](const json& p, const std::string& at) {
            if (!c.Object(p, at, {"record", "place"}))
                return;
            c.Int(p, "record", at, 0, 1 << 24);
            c.Numbers(p, "place", at, 12, 1e7);
        });
        if (o.contains("import") && c.Object(o["import"], w + ".import", {"level", "mesh"})) {
            c.Name(o["import"], "level", w + ".import", 16, kLevelChars);
            c.Int(o["import"], "mesh", w + ".import", 1, 0xFFFFFFFFLL);
        }
        if (o.contains("map_icon")) {
            if (!o["map_icon"].is_string() || std::find(icons.begin(), icons.end(), o["map_icon"].get<std::string>()) == icons.end())
                c.Bad(w + ".map_icon", "not an icon of the map's legend");
            else if (++mapIcons == kMaxMapIcons + 1)
                c.Bad(w + ".map_icon", "more than " + std::to_string(kMaxMapIcons) + " points on the map");
        }
    });
    std::set<std::string> npcIds;
    c.Array(j, "npcs", "", kMaxNpcs, [&](const json& o, const std::string& w) {
        if (!c.Object(o, w, {"pos", "matrix", "id", "label", "record", "model", "import_level", "import_mesh",
                             "import_name", "behaviour", "behaviour_level", "params", "stage", "gone_from"}))
            return;
        c.Place(o, w);
        c.Span(o, w);
        c.Name(o, "id", w, 64, "_-");
        c.Name(o, "label", w, 40, "_- ");  // its name in the editor and for scripts ("Volkov")
        if (o.contains("id") && o["id"].is_string() && !npcIds.insert(o["id"].get<std::string>()).second)
            c.Bad(w + ".id", "twice");
        c.Int(o, "record", w, -1, 1 << 24);
        c.Name(o, "model", w, 64, "_-");
        c.Name(o, "import_level", w, 16, kLevelChars);
        c.Int(o, "import_mesh", w, 0, 0xFFFFFFFFLL);
        c.Name(o, "import_name", w, 64, "_-");
        c.Text(o, "behaviour", w, 256);  // (the game's own path of it: "scripts\C1\C1_1\..\..\AllLevels\...")
        c.Name(o, "behaviour_level", w, 16, kLevelChars);
        c.Params(o, w);
    });
    c.Array(j, "npc_edits", "", kMaxNpcs, [&](const json& o, const std::string& w) {
        if (!c.Object(o, w, {"record", "path", "behaviour", "params"}))
            return;
        c.Int(o, "record", w, 0, 1 << 24);
        c.Text(o, "path", w, 512);
        c.Text(o, "behaviour", w, 256);
        c.Params(o, w);
    });
    if (j.contains("mission"))
        CheckMission(c, j["mission"]);
    if (j.contains("music")) {
        const json& m = j["music"];
        if (!m.is_object())
            c.Bad("level.music", "not an object");
        else if (m.size() > 64)
            c.Bad("level.music", "more than 64 places");
        else
            for (auto it = m.begin(); it != m.end(); ++it)
                if (it.key().size() > 512 || !it.value().is_string() || it.value().get_ref<const std::string&>().size() > 128)
                    c.Bad("level.music." + Checker::Clip(it.key()), "not a place and a track's name");
    }
    c.Array(j, "vehicles", "", kMaxVehicles, [&](const json& v, const std::string& w) { CheckVehicle(c, v, w); });
    // doors of the map locked / unlocked by the level
    c.Array(j, "doors", "", 256, [&](const json& o, const std::string& w) {
        if (!c.Object(o, w, {"path", "locked"}))
            return;
        if (!o.contains("path"))
            c.Bad(w, "no path");
        c.Text(o, "path", w, 512);
        if (o.contains("locked") && !o["locked"].is_boolean())
            c.Bad(w + ".locked", "not true / false");
    });
    c.Array(j, "new_lists", "", kMaxLists, [&](const json& o, const std::string& w) {
        if (!c.Object(o, w, {"name", "copy_of", "record"}))
            return;
        c.Name(o, "name", w, 64, "_-");
        c.Text(o, "copy_of", w, 512);
        c.Int(o, "record", w, 0, 1 << 24);
    });
    c.Array(j, "lists", "", kMaxLists, [&](const json& o, const std::string& w) {
        if (!c.Object(o, w, {"path", "points"}))
            return;
        c.Text(o, "path", w, 512);
        c.Array(o, "points", w, kMaxListPoints, [&](const json& p, const std::string& at) {
            if (!p.is_string() || p.get_ref<const std::string&>().size() > 512)
                c.Bad(at, "not a path");
        });
    });
    std::set<std::string> scripts;
    c.Array(j, "scripts", "", kMaxScripts, [&](const json& s, const std::string& w) {
        if (!s.is_string() || !ValidScriptName(s.get<std::string>()))
            c.Bad(w, "not a plain .lua name of the level's folder");
        else if (!scripts.insert(s.get<std::string>()).second)
            c.Bad(w, "twice");
    });
    c.Array(j, "imports", "", kMaxImports, [&](const json& o, const std::string& w) {
        if (!c.Object(o, w, {"level", "mesh", "name", "character", "anim_set", "outfit_mesh", "nude_mesh"}))
            return;
        c.Name(o, "level", w, 16, kLevelChars);
        c.Int(o, "mesh", w, 1, 0xFFFFFFFFLL);
        c.Text(o, "name", w, 128);  // (what the editor shows: "C3-3 SkyDome")
        c.Bool(o, "character", w);
        c.Name(o, "anim_set", w, 64, "_-");
        c.Int(o, "outfit_mesh", w, 0, 0xFFFFFFFFLL);
        c.Int(o, "nude_mesh", w, 0, 0xFFFFFFFFLL);
    });
    return r;
}

Report CheckLevelText(const std::string& text, bool strict)
{
    if (text.size() > kMaxFileBytes) {
        Report r;
        r.issues.push_back({true, "level.json", "larger than 16 MB"});
        return r;
    }
    json j = json::parse(text, nullptr, false);  // (no exceptions: a parse error is a discarded value)
    if (j.is_discarded()) {
        Report r;
        r.issues.push_back({true, "level.json", "not JSON"});
        return r;
    }
    return CheckLevel(j, strict);
}

} // namespace h2::levelfmt
