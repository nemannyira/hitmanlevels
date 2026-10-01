#include "model_catalog.h"

#include <cctype>
#include <vector>

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
        {Category::Helpers,
         {"gate", "insidebound", "bound", "coli", "camcoli", "pos", "pathfinder", "blocksight", "blocked", "seamfixer",
          "seemfixer", "flowtracer", "lod", "meaningless", "omni", "spot", "fspot", "icon", "text", "reticle",
          "crosshair", "compass", "scrolling", "bitmaps", "greenline", "button", "letters", "raster", "zoombox",
          "overlay", "hilight", "lens", "cursor", "levdef", "repos", "zinstance", "restrictedarea", "emitter",
          "particelemitter", "exhaustemitter", "stripcylinder", "pshadow", "shadow", "shad", "dummy", "helper",
          "marker", "trigger", "waypoint", "node", "plane", "core", "inner", "done", "blendcylinder", "riverspline",
          "entranceportal", "glow", "glows", "lighteffect", "lensflare", "sunbeam", "mesh"}},
        {Category::Effects,
         {"particle", "partice", "smoke", "spark", "flash", "flame", "fire", "rain", "snow", "cloud", "blood",
          "bullet", "cartrigde", "cartridge", "waterrings", "breath", "fragment", "splinter", "woodsplinter",
          "shatter", "collapse", "remains", "remaining", "piece", "debris", "decal", "housedecal", "explosion",
          "steam", "dust", "puddle"}},
        {Category::Vehicles,
         {"car", "truck", "train", "wagon", "wheel", "backwheel", "chassis", "heli", "helicopter", "boat", "crane",
          "bike", "tractor", "jeep", "van", "bus", "tank", "trackmotor", "exhaustflap"}},
        {Category::Lighting,
         {"lamp", "chandelier", "candle", "lantern", "bulb", "bulp", "lamppost", "lampstring", "light", "landinglight",
          "torch", "neon"}},
        {Category::Nature,
         {"tree", "bush", "grass", "outsidegrass", "plant", "planthall", "flower", "leaf", "leafs", "waterleaves",
          "lotus", "rock", "stone", "cliff", "hills", "soil", "log", "bamboo", "bamboopole", "pebbles", "water",
          "river", "sky", "skydome", "skyline", "cityscape", "background", "sun", "hedge", "palm", "mountain",
          "terrain", "dirt", "grounddirt", "mud", "sand", "ice"}},
        {Category::Furniture,
         {"chair", "table", "tabletop", "desk", "bed", "couch", "sofa", "shelf", "cabinet", "kitchencabinet", "closet",
          "locker", "lockerdoor", "bench", "trainbench", "carpet", "floormat", "rug", "mirror", "painting", "poster",
          "curtain", "blinds", "toilet", "shower", "zink", "sink", "tap", "stove", "kitchen", "kitchenbase",
          "refrigerator", "fridge", "counter", "wardrobe", "drawer", "chest", "dresser", "armchair", "stool",
          "goldframe", "bath", "piano", "fireplace", "clock"}},
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
          "leninbust", "flag", "map", "globe", "tv", "computer", "keyboard", "lamp", "plank", "tube", "cylinder",
          "chest", "pin", "joystick", "scanner", "handlebar", "holder"}},
        {Category::Architecture,
         {"wall", "mwall", "wallpanel", "floor", "flooredge", "ceiling", "roof", "roofedge", "tinroof", "stairs",
          "stair", "stairframe", "step", "steps", "stepplate", "steppingstone", "column", "pillar", "arch", "arcade",
          "balcony", "balchony", "window", "windows", "windowframe", "windowround", "windowblind", "windowkarm",
          "smallwindownew", "win", "glass", "door", "dooor", "doar", "dor", "vdoor", "portdooor", "dframe", "ddframe",
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
          "wagonsection", "wagonend", "poles", "pole", "arch", "gateway", "wallcorner", "seam", "block"}},
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

} // namespace

const char* CategoryName(Category c)
{
    switch (c) {
    case Category::Architecture: return "Architecture";
    case Category::Furniture: return "Furniture";
    case Category::Lighting: return "Lighting";
    case Category::Nature: return "Nature & terrain";
    case Category::Vehicles: return "Vehicles";
    case Category::Items: return "Weapons & items";
    case Category::Props: return "Props";
    case Category::Effects: return "Effects & debris";
    case Category::Other: return "Other";
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
