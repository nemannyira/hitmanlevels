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
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace h2::engine {

struct SceneNpc {
    std::string name;      // the actor's object name, unique in the scene ("NPC_Maid_1")
    int record = -1;       // the template character's actor record
    float mat[9] = {0, 0, 1, 0, 1, 0, 1, 0, 0};  // world orientation of its group (rows forward, up, right)
    float pos[3] = {};     // world position of its group
    uint32_t mesh = 0;     // another body (0: the template's)
    std::vector<std::string> anims;  // its animation sets (empty: the template's)
};

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
