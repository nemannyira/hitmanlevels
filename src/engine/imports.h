#pragma once
// Models of other levels in the loaded one (see docs/models.md).
//
// While a scene loads, ZEngineDataBase::AllocSequence reads its textures
// (TEX: GetTextureSize / GetTextureData, then the renderer) and geometry
// (PRM: GetPrimsSize / GetPrimsData into a buffer with 0x60000 bytes for
// runtime prims, then the renderer). Imported models are copied after that
// reserve: their records (0x84 bytes, chained by +0x08, one per material)
// and data (vertices, indices, bounds) with the offsets moved, their
// textures added to the TEX under free ids (or this level's texture of the
// same name). Collision (record +0x30, data in the level's OCT) is not
// copied yet.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace h2::engine {

struct ZGEOM;

struct ModelImport {
    std::string level;  // e.g. "C2-1"
    uint32_t mesh = 0;  // mesh reference in that level (a record of its PRM)
};

void InstallImports();
// What a scene being loaded gets ("C1-1\C1-1__MAIN"; asked while it loads).
void SetImportProvider(std::function<std::vector<ModelImport>(const std::string& scene)> provider);
// Testing: models for every level scene loaded from now on (besides the provider's).
void SetModelImports(const std::vector<ModelImport>& list);
// The mesh reference of an imported model in the loaded scene (0: not there).
uint32_t ImportedMesh(const std::string& level, uint32_t mesh);

// A model of another level, from its scene file.
struct LevelModel {
    uint32_t mesh = 0;
    std::string name;        // of the first object using it
    std::string group;       // that object's group (where it is in the level)
    uint32_t classId = 0;
    uint32_t flags = 0;      // node flags of that object
    float mat[9] = {};       // its rotation in the world
    float radius = 0;        // bounding sphere
    int uses = 0;
    int parts = 0;           // materials
    bool collision = false;  // it has collision data (record +0x30)
};
// The models of a level (cached; empty when it cannot be read). Only plain
// objects with a model of their own (no characters, no helpers).
const std::vector<LevelModel>& LevelModels(const std::string& level);
// Levels of the game ("C1-1", ...): folders of Scenes with a __MAIN.ZIP.
const std::vector<std::string>& GameLevels();

} // namespace h2::engine
