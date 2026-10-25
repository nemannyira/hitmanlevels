#include "model_catalog.h"

#include <windows.h>

#include <cctype>
#include <fstream>
#include <map>
#include <vector>

#include "json.hpp"

#include "../log.h"

namespace h2::editor {

namespace {

// Keywords match the start of a word of the name (lower case). Checked in
// this order, the first category with a match wins (helpers and effects
// first: "LampOmni" is a light source helper, "WallCollapse" debris).
struct Rule {
    Category category;
    std::vector<const char*> words;
};

const std::vector<Rule>& Rules()
{
    static const std::vector<Rule> rules = {
        {Category::Effects,
         {"caustics", "shadow", "shaodw", "shodow", "pshadow", "shad", "cloud", "glow", "glows", "lighteffect", "lensflare",
          "sunbeam"}},
        {Category::Helpers,
         {"gate", "insidebound", "bound", "coli", "camcoli", "pos", "pathfinder", "blocksight", "blocked", "seamfixer",
          "seemfixer", "flowtracer", "lod", "meaningless", "omni", "spot", "fspot", "icon", "text", "reticle",
          "crosshair", "compass", "scrolling", "bitmaps", "greenline", "button", "letters", "raster", "zoombox",
          "overlay", "hilight", "lens", "cursor", "levdef", "repos", "zinstance", "restrictedarea", "emitter",
          "particelemitter", "exhaustemitter", "stripcylinder", "dummy", "helper", "marker", "trigger", "waypoint",
          "node", "plane", "core", "inner", "outer", "done", "blendcylinder", "riverspline", "spline", "splinepath",
          "entranceportal", "mesh", "help", "helppoint", "point", "safepoint", "warp", "wpt", "wp", "patrol",
          "distract", "protect", "hidepoint", "arrow", "map", "frame", "border", "title", "font", "headline",
          "subtitles", "outline", "focus", "backlight", "selection", "selectionbar", "confirmation", "rating",
          "sizeexpand", "val", "piss", "walkto", "wander", "kill", "movetocreation", "intermediatestorageposition"}},
        {Category::Effects,
         {"particle", "partice", "smoke", "spark", "flash", "flame", "fire", "rain", "snow", "cloud", "blood",
          "bullet", "cartrigde", "cartridge", "waterrings", "breath", "fragment", "splinter", "woodsplinter",
          "shatter", "collapse", "remains", "remaining", "piece", "debris", "decal", "housedecal", "explosion",
          "steam", "dust", "puddle"}},
        {Category::Vehicles,
         {"car", "truck", "train", "wagon", "wheel", "backwheel", "chassis", "heli", "helicopter", "boat", "crane",
          "bike", "tractor", "jeep", "van", "bus", "tank", "trackmotor", "exhaustflap", "karros", "karroseri",
          "forskaerme", "bagskaerme", "skaerm", "seats", "seat", "vdoor", "bumper", "windshield", "steering",
          "exhaust", "ferrari", "limo", "limousine", "mercedes", "motorbike", "rotor", "blades", "cabine", "motor",
          "throttle"}},
        {Category::Lighting,
         {"lamp", "chandelier", "candle", "lantern", "bulb", "bulp", "lamppost", "lampstring", "light", "landinglight",
          "torch", "neon"}},
        {Category::Nature,
         {"tree", "bush", "grass", "outsidegrass", "plant", "planthall", "flower", "leaf", "leafs", "waterleaves",
          "lotus", "rock", "stone", "cliff", "hills", "soil", "log", "bamboo", "bamboopole", "pebbles", "water",
          "river", "sky", "skydome", "skyline", "cityscape", "background", "sun", "hedge", "palm", "mountain",
          "terrain", "dirt", "grounddirt", "mud", "sand", "ice", "ocean", "sea", "lake"}},
        {Category::Furniture,
         {"chair", "table", "tabletop", "desk", "bed", "couch", "sofa", "shelf", "cabinet", "kitchencabinet", "closet",
          "locker", "lockerdoor", "bench", "trainbench", "carpet", "floormat", "rug", "mirror", "painting", "poster",
          "curtain", "blinds", "toilet", "shower", "zink", "sink", "tap", "stove", "kitchen", "kitchenbase",
          "refrigerator", "fridge", "counter", "wardrobe", "drawer", "chest", "dresser", "armchair", "stool",
          "goldframe", "bath", "piano", "fireplace", "clock", "oven", "shelve", "shelves", "bookcase", "blanket", "futon", "pillow", "coat", "washingmachine",
          "divider", "forhang", "workoutmat"}},
        {Category::Items,
         {"ammo", "gun", "pistol", "rifle", "shotgun", "sniper", "knife", "sword", "katana", "bomb", "smokebomb",
          "grenade", "key", "binocs", "goggles", "syringe", "suitcase", "briefcase", "weapon", "magazine", "wire",
          "garrote", "poison", "camera", "radio", "phone", "tool", "kitchentool", "hammer", "uzi", "ak", "mp",
          "silencer", "scope", "medkit"}},
        {Category::Props,
         {"box", "crate", "cratte", "woodbox", "blackbox", "barrel", "bottle", "champagnebottle", "vase", "pot",
          "basket", "basketwicker", "book", "books", "paper", "paperelement", "scroll", "cup", "mug", "plate", "bowl",
          "bowlsushi", "waterbowl", "spoon", "chopstick", "can", "tomatocan", "food", "goods", "bundle", "sack", "bag",
          "dumpster", "ashtray", "laundry", "towel", "dishrag", "cord", "powercord", "pipe", "pipeline", "sign",
          "exitsign", "monitor", "screen", "switchbox", "antenna", "bell", "churchbell", "watermelon", "mangoes",
          "potatochips", "suger", "contents", "tie", "manhole", "trash", "bin", "can", "jar", "statue", "bust",
          "leninbust", "flag", "globe", "tv", "computer", "keyboard", "lamp", "plank", "tube", "cylinder",
          "chest", "pin", "joystick", "scanner", "handlebar", "holder", "bucket", "cardboxes", "fish", "fruit",
          "fruitpile", "cherries", "grapes", "grain", "onion", "cloth", "clothstack", "buddha", "speaker", "logo"}},
        {Category::Architecture,
         {"wall", "mwall", "wallpanel", "floor", "flooredge", "ceiling", "roof", "roofedge", "tinroof", "stairs",
          "stair", "stairframe", "step", "steps", "stepplate", "steppingstone", "column", "pillar", "arch", "arcade",
          "balcony", "balchony", "window", "windows", "windowframe", "windowround", "windowblind", "windowkarm",
          "smallwindownew", "win", "glass", "door", "dooor", "doar", "dor", "portdooor", "dframe", "ddframe",
          "wframe", "fence", "fencemidsection", "railing", "handrail", "handrailend", "beam", "outerbeam", "rafter",
          "wallrafter", "frieze", "friezecorner", "friezecolum", "facade", "facadesouth", "facadenorth", "foundation",
          "building", "buildingbase", "house", "housewall", "housestep", "tower", "chimney", "porch", "veranda",
          "verandaroof", "landing", "ladder", "sewerladder", "elevator", "elevatorbox", "lift", "escalator",
          "escalatorstep", "escalatorhandrail", "escalatorwood", "tiles", "tilefloor", "panel", "shutter", "entrance",
          "entre", "portal", "alcove", "alcovewall", "alcovewalls", "corner", "wallcorner", "housecornerout", "base",
          "sewer", "sewersection", "sewerfence", "subway", "subwaytracks", "subwaycolumn", "strack", "track", "road",
          "roadsection", "roadcurve", "roadblock", "groad", "asphalt", "driveway", "drive", "path", "pavement",
          "sidewalk", "ground", "gound", "groundsect", "pool", "well", "shed", "booth", "stall", "stallcover",
          "bazaarwall", "bazaarroof", "bazaarglass", "bazaarbaresection", "endwall", "upperwall", "blindwall",
          "seperatorwall", "separatorwall", "woodenwall", "wallwooda", "wallwoodb", "wallhole", "wallcityouter",
          "guardhousewall", "houseguardroof", "complexceiling", "floordecor", "helopad", "helopadrim", "backyard",
          "porch", "bridge", "platform", "pier", "dock", "wood", "brick", "concrete", "metal", "slisk", "trainfloor",
          "wagonsection", "wagonend", "poles", "pole", "arch", "gateway", "wallcorner", "seam", "block", "ramp",
          "vent", "lobby", "corridor", "hall", "room", "markise",
          "rail", "loft"}},
    };
    return rules;
}

std::vector<std::string> Words(const std::string& name)
{
    std::vector<std::string> words;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty())
            words.push_back(cur);
        cur.clear();
    };
    for (size_t i = 0; i < name.size(); ++i) {
        unsigned char c = name[i];
        if (!std::isalpha(c)) {
            flush();
            continue;
        }
        // CamelCase boundary: "LampOmni" -> lamp, omni (not inside ALLCAPS runs)
        bool upper = std::isupper(c) != 0;
        bool prevLower = i > 0 && std::islower(static_cast<unsigned char>(name[i - 1]));
        if (upper && prevLower)
            flush();
        cur += char(std::tolower(c));
    }
    flush();
    return words;
}

bool Matches(const std::vector<std::string>& words, const std::string& joined, const char* key)
{
    std::string k = key;
    for (const std::string& w : words)
        if (w == k)
            return true;
    // whole name as one word ("insidebound", "wallpanel") or a word starting with
    // a longer keyword ("windows" for "window"); short keywords only as words
    if (k.size() >= 5 && joined.rfind(k, 0) == 0)
        return true;
    for (const std::string& w : words)
        if (k.size() >= 4 && w.rfind(k, 0) == 0)
            return true;
    return false;
}

// the user's corrections: model name or family -> category
std::map<std::string, Category> g_overrides;
bool g_overridesLoaded;

std::string OverridesFile() { return GameDir() + "Mods\\model_categories.json"; }

void LoadOverrides()
{
    g_overridesLoaded = true;
    std::ifstream f(OverridesFile(), std::ios::binary);
    if (!f)
        return;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_object())
        return;
    for (auto& [key, value] : j.items()) {
        if (!value.is_string())
            continue;
        for (int c = 0; c < int(Category::Count); ++c)
            if (value.get<std::string>() == CategoryKey(Category(c)))
                g_overrides[key] = Category(c);
    }
}

void SaveOverrides()
{
    nlohmann::json j = nlohmann::json::object();
    for (auto& [key, c] : g_overrides)
        j[key] = CategoryKey(c);
    CreateDirectoryA((GameDir() + "Mods").c_str(), nullptr);
    std::ofstream f(OverridesFile(), std::ios::binary | std::ios::trunc);
    if (f)
        f << j.dump(2);
    else
        Log("cannot write %s", OverridesFile().c_str());
}

} // namespace

const char* CategoryKey(Category c)
{
    switch (c) {
    case Category::Pickup: return "Pickup";
    case Category::Architecture: return "Architecture";
    case Category::Furniture: return "Furniture";
    case Category::Lighting: return "Lighting";
    case Category::Nature: return "Nature";
    case Category::Vehicles: return "Vehicles";
    case Category::Items: return "Items";
    case Category::Props: return "Props";
    case Category::Effects: return "Effects";
    case Category::Other: return "Other";
    case Category::Interface: return "Interface";
    case Category::Helpers: return "Helpers";
    default: return "?";
    }
}

bool CategoryOverride(const std::string& key, Category* out)
{
    if (!g_overridesLoaded)
        LoadOverrides();
    auto it = g_overrides.find(key);
    if (it == g_overrides.end())
        return false;
    if (out)
        *out = it->second;
    return true;
}

void SetCategoryOverride(const std::string& key, Category c)
{
    if (!g_overridesLoaded)
        LoadOverrides();
    if (c == Category::Count)
        g_overrides.erase(key);
    else
        g_overrides[key] = c;
    SaveOverrides();
}

const char* CategoryName(Category c)
{
    switch (c) {
    case Category::Pickup: return "Pickup items (working)";
    case Category::Architecture: return "Architecture";
    case Category::Furniture: return "Furniture";
    case Category::Lighting: return "Lighting";
    case Category::Nature: return "Nature & terrain";
    case Category::Vehicles: return "Vehicles";
    case Category::Items: return "Weapons & items";
    case Category::Props: return "Props";
    case Category::Effects: return "Effects & debris";
    case Category::Other: return "Other";
    case Category::Interface: return "Interface & off-level";
    case Category::Helpers: return "Helpers (invisible)";
    default: return "?";
    }
}

Category Categorize(const std::string& name, const std::string& className)
{
    if (className.find("Gate") != std::string::npos || className == "ZBOUND" ||
        className.find("Light") != std::string::npos || className.find("Sound") != std::string::npos)
        return Category::Helpers;
    std::vector<std::string> words = Words(name);
    std::string joined;
    for (const std::string& w : words)
        joined += w;
    for (const Rule& r : Rules())
        for (const char* k : r.words)
            if (Matches(words, joined, k))
                return r.category;
    return Category::Other;
}

std::string FamilyOf(const std::string& name)
{
    // up to the first digit or "_<digit>"; CamelCase words after the first stay
    // ("WallPanel" is its own family), "MWall" too
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
        unsigned char c = name[i];
        if (std::isdigit(c))
            break;
        if ((c == '_' || c == ' ' || c == '-') && i + 1 < name.size() &&
            (std::isdigit(static_cast<unsigned char>(name[i + 1])) || !out.empty()))
            break;
        out += char(c);
    }
    while (!out.empty() && (out.back() == '_' || out.back() == ' ' || out.back() == '-'))
        out.pop_back();
    return out.empty() ? name : out;
}

} // namespace h2::editor
