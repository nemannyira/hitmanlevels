#include "npc_docs.h"

#include <cctype>
#include <cstring>
#include <map>
#include <vector>

namespace h2::editor {

namespace {

// Parameters shared by many behaviours (counts over all levels in brackets in
// the comments: how many NPCs of the game set them).
const std::map<std::string, std::string>& Known()
{
    static const std::map<std::string, std::string> docs = {
        // every character (717)
        {"m_iHitpoints", "Health: how much damage the character takes before dying. 20 is a normal person; "
                         "bosses and bodyguards often have more."},
        {"Mute", "1 = the character never speaks (no shouts, remarks or dialogue)."},
        // armed characters (524)
        {"WeaponTemplate", "The weapon the character carries: an item template (in the scene's ItemTemplates). "
                           "None = unarmed. Select a template in the Scene tree and press \"= selected\"."},
        {"MayHolsterWeapon", "1 = puts the weapon away while calm; 0 = always holds it."},
        {"ReactionTime", "Seconds before reacting to something suspicious or hostile; 0 = the default."},
        {"m_Arbiter", "Guard coordinator (GuardArbiter): guards that share one search, alarm and attack "
                      "together. Normally the level's arbiter; keep it as it is."},
        // guards (491-499)
        {"GuardPoint", "The place the guard stands at and returns to when calm. None: stays where it was placed."},
        {"IsSentry", "1 = stationary sentry: does not leave its post, reacts only within SentryRadius."},
        {"SentryRadius", "Radius in cm around the guard point that a sentry watches and moves in (500 = 5 m)."},
        // patrols (154-193)
        {"WayPoints", "The patrol route: a waypoint list (list object in the scene) walked in order. "
                      "Shown with \"Routes\"."},
        {"WaitTime", "Seconds the character stops at each point of its route."},
        {"Delay", "Delay before the character starts walking its route."},
        {"StartPathPos", "Index of the route point the character starts at (0 = the first)."},
        {"WarpToPathPoint", "1 = put onto its start point when the level starts; 0 = walks there from where it is."},
        {"PatrolRelaxed", "1 = calm patrol (slower, relaxed animations); 0 = alert patrol."},
        {"RelaxedPatrol", "1 = calm patrol (slower, relaxed animations); 0 = alert patrol."},
        {"PauseAtPoint", "1 = stops for a while at the pause points of the route."},
        {"PausePoints", "List of route points where the character stops for a while."},
        {"LoopPath", "1 = goes on from the last route point to the first again."},
        {"ReversePathOnCompletion", "1 = walks the route back and forth."},
        {"bRandomPath", "1 = visits the route points in random order."},
        {"patrolPointList", "The patrol route: a list of points walked in order."},
        {"m_rPatrolPositionList", "The patrol route: a list of points walked in order."},
        {"Waypoints", "The route: a list of points walked in order."},
        {"waypoints", "The route: a list of points walked in order."},
        {"PathList", "The route: a list of points walked in order."},
        // civilians (148)
        {"HelpPoints", "List of places the character runs to for help (to fetch guards) when it sees a crime "
                       "or a body."},
        {"SafePoints", "List of places the character flees to when panicking."},
        {"rLevelControl", "The level's control object (LevelControl script): the character reports mission "
                          "events to it. Keep it as it is."},
        {"lvlControl", "The level's control object (LevelControl script). Keep it as it is."},
        // pedestrians (36)
        {"WayPointList", "Points the character walks between (pedestrians)."},
        {"isWPListPath", "1 = walks the points in order as a path; 0 = picks the next point at random."},
        {"startAtRandomPathPoint", "1 = starts at a random point of the list."},
        {"SkipProbability", "Chance (0-1) to skip a point of the list."},
        {"WalkPositions", "Points the character walks between."},
        {"WalkAroundList", "Points the character strolls between when bWalkAround is on."},
        {"bWalkAround", "1 = strolls between the points of WalkAroundList."},
        {"WalkAboutPoints", "Points the character strolls between."},
        // suspicion (22-42)
        {"RunAnnoyance", "How suspicious running near this guard is (1 = normal, more = notices sooner, "
                         "0 = ignores it)."},
        {"SneakAnnoyance", "How suspicious sneaking near this guard is (1 = normal, more = notices sooner, "
                           "0 = ignores it)."},
        {"TrespassAnnoyance", "How suspicious being in a forbidden area near this guard is (1 = normal, "
                              "more = notices sooner, 0 = ignores it)."},
        {"Xenophobia", "Suspicion of unknown people (e.g. Hitman in a disguise): 0 = none, higher = sooner."},
        {"ProtectPoint", "The place or person the guard protects: stays near it and returns to it."},
        {"m_PanicArbiter", "Coordinator of panic reactions (who flees or calls guards). Keep it as it is."},
        {"m_rDistractPoint", "Where the guard goes when it is distracted (e.g. by a noise)."},
        {"AlarmPoint", "Where the character runs to raise the alarm."},
        {"m_AlarmList", "List of alarm points or switches the guard uses to raise the alarm."},
        {"rAlarmPos", "Where the character runs to raise the alarm."},
        {"rAlarmRelay", "Object that passes the alarm on to other guards."},
        // looks and voice
        {"BreathEnabled", "1 = visible breath in cold air."},
        {"bEnableBreath", "1 = visible breath in cold air."},
        {"UseGoggles", "1 = uses night vision goggles."},
        {"bFemale", "1 = female (voice and animations)."},
        {"iFemale", "1 = female (voice and animations)."},
        {"m_SoundSet", "Set of voice lines the character uses."},
        {"m_lVoicePitch", "Voice pitch shift (0 = normal)."},
        // C1-1 (Sicilian mansion)
        {"PostManAllowed", "C1-1: 1 = lets the postman through."},
        {"AtKitchenGate", "C1-1: 1 = this guard is the one at the kitchen gate."},
        {"KitchenGuard", "1 = this guard belongs to the kitchen."},
        // others seen in several levels
        {"m_rStartPos", "Start position of the character."},
        {"FollowPoints", "Points the character follows / goes through."},
        {"SewerPoints", "Points a sewer rat runs between."},
        {"fMaxSniperAttackDistance", "Farthest distance in cm the character shoots at Hitman from."},
        {"rListOutsideBoxes", "Areas that count as \"outside\" for this guard (where Hitman is allowed)."},
        {"On2ndFloor", "1 = the character is on the 2nd floor (level script logic)."},
        {"rToilet", "Toilet the character visits now and then."},
        {"rChair", "Chair the character sits on."},
        {"SniperLookPosition", "Where the sniper looks / aims."},
    };
    return docs;
}

// "rKitchenEntrance" -> "kitchen entrance"; drops the usual prefixes
// (m_, r, b, i, f, c, p, is, m_r).
std::string Words(std::string name)
{
    if (name.rfind("m_", 0) == 0)
        name = name.substr(2);
    for (const char* prefix : {"is", "r", "b", "i", "f", "c", "p", "l", "s"}) {
        size_t n = strlen(prefix);
        if (name.size() > n && name.rfind(prefix, 0) == 0 && std::isupper(static_cast<unsigned char>(name[n]))) {
            name = name.substr(n);
            break;
        }
    }
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (c == '_') {
            out += ' ';
            continue;
        }
        bool upper = std::isupper(static_cast<unsigned char>(c)) != 0;
        bool prevLower = i > 0 && std::islower(static_cast<unsigned char>(name[i - 1]));
        if (upper && prevLower && !out.empty() && out.back() != ' ')
            out += ' ';
        out += char(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool Has(const std::string& lower, std::initializer_list<const char*> keys)
{
    for (const char* k : keys)
        if (lower.find(k) != std::string::npos)
            return true;
    return false;
}

std::string Lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

ParamDoc DescribeParam(const std::string& name, const std::string& type)
{
    auto it = Known().find(name);
    if (it != Known().end())
        return {it->second, false};
    std::string w = Words(name);
    std::string l = Lower(name);
    ParamDoc d;
    d.inferred = true;
    if (type == "ZGEOMREF") {
        if (Has(l, {"list", "points", "positions", "wps", "waypoints", "path", "guards", "locations", "chairs"}))
            d.text = "List used by this behaviour: " + w + ". A list object of the scene; if it holds points, "
                     "they are drawn with \"Routes\".";
        else if (Has(l, {"guard", "boss", "friend", "partner", "colleage", "agent", "officer", "chef", "maid",
                         "nurse", "lieutenant", "colonel", "victim", "patient", "postman", "lawyer", "driver",
                         "bodyguard", "concubine", "spetsnaz", "fireman", "counterpart", "pal", "opponent",
                         "ambassador", "guest", "clerk", "cia", "kgb", "hayamoto", "mafia", "vittorio"}))
            d.text = "Another character this one deals with: " + w + ".";
        else if (Has(l, {"pos", "point", "wp", "wpt", "spot", "location", "place", "start", "stop", "exit",
                         "escape", "walk", "hide", "piss", "smok", "sit", "seat", "stand", "look", "inside",
                         "outside", "room", "kitchen", "office", "garden", "balcony", "corridor", "stair",
                         "elevator", "door", "gate", "window", "chair", "desk", "toilet", "sink", "bed"}))
            d.text = "A place the character goes to or uses: " + w + ". Point it at a marker or object.";
        else
            d.text = "Object used by this behaviour: " + w + ".";
    } else if (type == "LONG") {
        if (Has(l, {"nr", "number", "count", "id", "type", "seat", "index"}))
            d.text = "Number: " + w + ".";
        else
            d.text = "Switch (0 = off, 1 = on): " + w + ".";
    } else if (type == "FLOAT") {
        if (Has(l, {"time", "delay", "interval", "wait"}))
            d.text = "Time in seconds: " + w + ".";
        else if (Has(l, {"dist", "radius", "range"}))
            d.text = "Distance in cm: " + w + ".";
        else if (Has(l, {"prob", "chance"}))
            d.text = "Chance (0-1): " + w + ".";
        else
            d.text = "Value: " + w + ".";
    } else if (type == "SndRef" || type == "SndSetRef") {
        d.text = "Sound the character uses: " + w + ".";
    } else {
        d.text = w + " (" + type + ").";
    }
    d.text += " Specific to this level's script: its exact effect is not known.";
    return d;
}

std::string ParamLabel(const std::string& name)
{
    static const std::pair<const char*, const char*> kLabels[] = {
        {"m_iHitpoints", "Hit points"},          {"Mute", "Silent"},
        {"WeaponTemplate", "Weapon"},            {"MayHolsterWeapon", "Puts weapon away"},
        {"ReactionTime", "Reaction time (s)"},   {"GuardPoint", "Guard point"},
        {"m_Arbiter", "Guard group"},            {"m_PanicArbiter", "Panic group"},
        {"IsSentry", "Sentry (stays)"},          {"SentryRadius", "Sentry radius (cm)"},
        {"WayPoints", "Route"},                  {"Waypoints", "Route"},
        {"waypoints", "Route"},                  {"PathList", "Route"},
        {"patrolPointList", "Route"},            {"WayPointList", "Route"},
        {"Delay", "Start delay (s)"},            {"StartPathPos", "Starts at point no."},
        {"WarpToPathPoint", "Starts on its route"}, {"WaitTime", "Wait at points (s)"},
        {"RunAnnoyance", "Running annoys"},      {"SneakAnnoyance", "Sneaking annoys"},
        {"TrespassAnnoyance", "Trespass annoys"}, {"Xenophobia", "Suspicion of strangers"},
        {"ProtectPoint", "Protects"},            {"m_rDistractPoint", "Distraction point"},
        {"HelpPoints", "Runs for help to"},      {"SafePoints", "Flees to"},
        {"LoopPath", "Route loops"},             {"PatrolRelaxed", "Calm patrol"},
        {"RelaxedPatrol", "Calm patrol"},        {"AlarmPoint", "Raises the alarm at"},
        {"PostManAllowed", "Lets the postman in (C1-1)"}, {"AtKitchenGate", "At the kitchen gate (C1-1)"},
        {"fMaxSniperAttackDistance", "Shoots from (cm)"},
    };
    for (const auto& [key, label] : kLabels)
        if (name == key)
            return label;
    return name;
}

std::string DescribeBehaviour(const std::string& shortName)
{
    std::string l = Lower(shortName);
    std::string what;
    if (Has(l, {"sniper"}))
        what = "Sniper: takes position and shoots from long range.";
    else if (Has(l, {"ninja"}))
        what = "Ninja guard (Japanese levels): guards or patrols its area, fights in close combat.";
    else if (Has(l, {"patrol"}))
        what = "Patrolling guard: walks its route (WayPoints / patrol list), stops at the points, investigates "
               "and raises the alarm or attacks when it sees Hitman.";
    else if (Has(l, {"sleep"}))
        what = "Sleeping guard: sleeps until woken by noise or alarm.";
    else if (Has(l, {"random"}))
        what = "Walks between random points of its list; reacts as its kind (guard or civilian).";
    else if (Has(l, {"pedestrian", "walker"}))
        what = "Pedestrian: walks between the points of its list, panics and flees when there is trouble.";
    else if (Has(l, {"rat", "pig", "dog", "fish", "boid", "animal"}))
        what = "Animal.";
    else if (Has(l, {"driver", "limo", "car", "van", "vehicle"}))
        what = "Driver or vehicle related character (works with the level's vehicles).";
    else if (Has(l, {"guard", "security", "soldier", "bodyguard", "officer", "spetsnaz", "unsoldier"}))
        what = "Guard: stands at its guard point (or protect point), investigates what is suspicious, raises "
               "the alarm, attacks.";
    else if (Has(l, {"guest", "waiter", "maid", "chef", "nurse", "merchant", "cleaner", "clerk", "programmer",
                     "technician", "postman", "delivery", "patient", "mother", "secretary", "civil", "neutral",
                     "member", "staff", "fireman"}))
        what = "Civilian: does its job or routine, panics and runs for help or flees at trouble.";
    else
        what = "Special character of this level (a target or a character of the mission's story).";
    return what + " Level-specific behaviours expect that level's objects (points, lists, doors) in their "
                  "parameters.";
}

} // namespace h2::editor
