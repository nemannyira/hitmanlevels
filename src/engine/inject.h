#pragma once
// Characters of a level made part of the scene file while it loads.
//
// An actor made at runtime (npc.h SpawnNpc) is not quite one of the scene's:
// its nodes come from the runtime pool, not the scene's static node pools,
// and several systems only know the actors the scene file had (Hitman bumps
// into them, bullets hit them). So a level's characters are added to the
// scene's records instead: while ZEngineDataBase::CreateGeoms reads the
// unpacked GMS and its BUF, copies of them get, for each character, the
// records of its template character (its group, the actor and its aim
// helper, ZAIMTHINGY) with the level's name, place and body, the geom
// counts and node pool counts they need, and the actor in the scene's
// list TrackLinkObjects. The engine then makes them as it makes the others.
// Their behaviour is set after loading (npc.h SetScript).
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace h2::engine {

struct ZGEOM;

struct SceneNpc {
    std::string name;      // the actor's object name, unique in the scene ("NPC_Maid_1")
    int record = -1;       // the template character's actor record
    float mat[9] = {0, 0, 1, 0, 1, 0, 1, 0, 0};  // world orientation of its group (rows forward, up, right)
    float pos[3] = {};     // world position of its group
    uint32_t mesh = 0;     // another body (0: the template's)
    std::vector<std::string> anims;  // its animation sets (empty: the template's)
    // Its outfits with another body: its clothes on Hitman and its body without
    // them (the template's data block names groups HitmanAs/<model>_HMas and
    // _Nude, each with a drawn child): made as records of their own with these
    // bodies, named <importName>_HMas / _Nude (0: the template's kept).
    uint32_t outfitMesh = 0, nudeMesh = 0;
    std::string importName;
    // Another icon on the in-game map ("Icon_Target"; "": the template's): its
    // group's ZGEOM_MapObject data (the game's characters' icons are there).
    std::string mapIcon;
};

// A new list of the scene (a route of a character's own): a copy of a list
// record (its elements too: the editor sets them after loading), at the root.
struct SceneList {
    std::string name;  // the list's object name, unique in the scene ("NPC_Guard_1_Route")
    int record = -1;   // the list it copies
};
// Asked for every scene that loads, with the characters.
void SetSceneListProvider(std::function<std::vector<SceneList>(const std::string& scene)> provider);

// A vehicle of the level's own, made as the game's are: a ZCar group (its
// mover drives it along a spline: SetPath / SetMoveParams / MoveToSplineVal)
// with copies of a model group's records (a car of the map: body, wheels,
// doors) as its children, and its spline (a ZSHAPE whose PRM record
// imports.h makes, SceneShape), both at the root.
struct SceneCar {
    std::string name;   // the car's group ("ROOT/<name>")
    std::string shape;  // its spline's object name (ShapeMesh of the same name)
    int model = -1;     // the group record it is a copy of (information)
    // its models (records of plain meshes, ZSTDOBJ) as the car's own children, in the car's frame
    // (mat 9, pos 3): the first is its body (ZMoveOnSpline's init takes the first plain child as
    // the body, +0xDE, which ZCar's placing 0x58FC20 needs; C1-1's van: Body)
    std::vector<std::pair<int, std::array<float, 12>>> parts;
    // where its wheels are (car frame: x right, y up, z forward): FrontLeft_01, FrontRight_01,
    // BackLeft_01, BackRight_01. ZCar's init (0x592EC0) finds its wheels by those names (the
    // front ones steer) and divides by their counts; a model with its wheels in one mesh gets
    // empty groups there (as the game's: C1-1's van has Wheel_FrontLeft_01 .. BackRight_01)
    std::vector<std::array<float, 3>> wheels;
    float mat[9] = {0, 0, 1, 0, 1, 0, 1, 0, 0};    // the car's place (world) until it drives
    float pos[3] = {};
};
void SetSceneCarProvider(std::function<std::vector<SceneCar>(const std::string& scene)> provider);

// A place of the level's goals, watched as the game watches its own (an exit, a
// room): a box (ZBoxPrimitive, class 0x2000E1, its half sizes) with a
// CheckInside routine, a copy of one of the scene's, at the root. The routine
// (ctor 0x48A830, data 0x4FC060: checked geom +0x72 (0: Hitman), receiver
// +0x76, enter / leave messages +0x7A / +0x7C) checks Hitman every frame
// (0x48ACA0, the box's vfunc 0x16C) and keeps whether he is inside at +0x88;
// it tells itself message "0" (the game's none).
struct SceneZone {
    std::string name;  // unique in the scene ("H2_Reach_1")
    float pos[3] = {};
    float half[3] = {};
};
void SetSceneZoneProvider(std::function<std::vector<SceneZone>(const std::string& scene)> provider);
// The record made for a zone of that name in the scene loaded last (-1: none).
int SceneZoneRecord(const std::string& name);
// Whether Hitman is inside it, as its CheckInside routine found (false: no such zone).
bool InSceneZone(const std::string& name);
// Whether Hitman is inside a zone (a box with a CheckInside routine, +0x88).
bool InZone(ZGEOM* box);

// A copy of one of the scene's models the level placed ("Crate_01+1"), made as
// the scene's own objects are: a copy of the model's record at the root, with
// the level's name and place. The game then treats it as its own (the actions
// its routines offer, saved games: its name is in the scene's strings). As a
// car's parts, it is a full node (0x200000: a plain model's record without it
// is a static node that must not move) and an empty light table entry until
// the level runs; dynamic and drawn with the rooms around it (0x44000, as the
// game's moving objects: a root object is in no room).
struct SceneCopy {
    std::string name;    // unique in the scene
    int record = -1;     // the model's record (a plain model, not a group)
    std::string source;  // that record's name (checked: another map version, another record)
    float mat[9] = {0, 0, 1, 0, 1, 0, 1, 0, 0};  // world (rows forward, up, right)
    float pos[3] = {};
    // A copy of a group of models (a car model of the map): an empty group here
    // with copies of these models' records in it, in its frame (mat 9, pos 3),
    // as a level's car's parts (record and source are not used then).
    std::vector<std::pair<int, std::array<float, 12>>> parts;
    // A copy of a model of another level (imports.h): a plain model record of
    // the scene with this model (its mesh in the loaded map) and node flags.
    uint32_t mesh = 0;
    uint32_t nodeFlags = 0;
    // A point of the level (characters walk to it): a copy of the characters'
    // aim helper record (ZAIMTHINGY: not drawn, not collided) at the root.
    bool point = false;
    // A point's icon on the in-game map (M): one of the map's legend's
    // (AllLevels/Interface/Map/Legend: "Icon_Exit", ...). A copy of one of the
    // map's own icon records (a ZBoxPrimitive with a ZGEOM_MapObject routine)
    // named "<name>_MapIcon" at the point, at the root; the editor puts it onto
    // the floor of its place once the map has loaded (as the map's own icons
    // are in their floors' groups). Not put into a room by its place: the
    // engine's rooms are large boxes (a point on C1-1's hill outside was in a
    // room of the mansion's 2nd floor).
    std::string mapIcon;
};
// The icons a point can have on the in-game map (the legend's, but Hitman's).
const std::vector<std::string>& MapIconNames();

// A model of the level drawn on the in-game map (ingame_map.h): a copy of one
// of the map's floor plan records (a plain model there: not in a room, so the
// world's cameras do not draw it and nothing collides with it) with this model,
// at the root, made with the map; once the map has loaded, the editor puts it
// onto the floor the map shows its object on, where the map's camera draws it.
struct SceneMapModel {
    std::string name;  // unique in the scene
    int record = -1;   // a plain model record of the scene whose model it has, or
    uint32_t mesh = 0; // a model of another level imported into this one (record -1)
    float mat[9] = {0, 0, 1, 0, 1, 0, 1, 0, 0};  // the map's space (rows forward, up, right)
    float pos[3] = {};
};
void SetSceneMapModelProvider(std::function<std::vector<SceneMapModel>(const std::string& scene)> provider);

// Another icon on the in-game map for the scene's own characters, made with
// the map: their groups' ZGEOM_MapObject data with that icon. Either one of
// them (`object`: the character's path as the editor names it, or its
// group's: a level's mission target, "Icon_Target"), or every one the scene
// file marks with the icon `from` (the base level's targets: "Icon_Enemy"
// when its mission no longer counts); these first.
struct SceneMapIcon {
    std::string object;
    std::string icon;
    std::string from;
};
void SetSceneMapIconProvider(std::function<std::vector<SceneMapIcon>(const std::string& scene)> provider);
void SetSceneCopyProvider(std::function<std::vector<SceneCopy>(const std::string& scene)> provider);
// The record made for a copy of that name in the scene loaded last (-1: none).
int SceneCopyRecord(const std::string& name);
// What the engine could not take of the level when the scene loaded last:
// objects left out (records with data past CreateGeoms' 0x2000) and moving
// objects not made so (no room list left: not drawn).
struct SceneLeftOut {
    int objects = 0;
    int notMoving = 0;
};
SceneLeftOut LevelLeftOut();
// Whether a record of the scene loaded last is one of those copies.
bool IsSceneCopyRecord(int index);
// Whether a record of the scene loaded last is the scene file's own (not one
// the level added: characters, lists, zones, cars, copies).
bool IsSceneFileRecord(int index);

// New behaviour settings of one of the scene's characters, made part of its
// record's script block while the scene loads: the script engine reads them as
// it reads the scene's own when it makes the character (its parameter parse).
// Values as the level keeps them: LONG / FLOAT numbers, STRING text, ZGEOMREF
// an object's path ("ROOT/.../Lobby"; the level's own objects made with the
// scene by their names), RAW hex.
struct SceneScriptEdit {
    int record = -1;      // the character's record (its script block)
    std::string script;   // its behaviour (the record's own; another is not made so)
    struct Value {
        std::string name, type, value;
    };
    std::vector<Value> values;
};
void SetSceneScriptEditProvider(std::function<std::vector<SceneScriptEdit>(const std::string& scene)> provider);
// Whether a record's settings were made with the scene loaded last.
bool IsSceneScriptEdited(int record);

// Asked for every scene that loads (its name as GetSceneName gives it).
void SetSceneNpcProvider(std::function<std::vector<SceneNpc>(const std::string& scene)> provider);
// The record made for a character of that name in the scene loaded last (-1: none).
int SceneNpcRecord(const std::string& name);
// Whether a record of the scene loaded last is one of those (not the scene file's).
bool IsSceneNpcRecord(int index);

void InstallSceneNpcs();
// scene_data.cpp: the GMS CreateGeoms reads (nullptr after).
void SetSceneNpcGms(const void* gms);
// Hitman starts at `pos` facing `mat` (world; rows forward, up, right) the
// next time the scene `scene` ("C1-1\C1-1__MAIN") loads, once.
void SetPlayerStart(const std::string& scene, const float pos[3], const float mat[9]);
// scene_data.cpp: the GMS copy of the loading scene `scene` gets Hitman's
// start (his group, the parent of the ZHITMAN2 record: its ancestors are
// at the origin on every level); true when it changed.
bool MovePlayerStart(const std::string& scene, std::vector<uint8_t>& gms, const std::vector<uint8_t>& buf);
// scene_data.cpp: the GMS / BUF copies of the loading scene `scene` get the
// characters; true when they changed.
bool AddSceneNpcs(const std::string& scene, std::vector<uint8_t>& gms, std::vector<uint8_t>& buf);

} // namespace h2::engine
