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

struct LevelModelPart {
    uint32_t mesh = 0;
    bool visible = true;  // drawn (record flag 1); else only its collision is used
    float mat[9] = {};  // in the group (as in the scene file)
    float pos[3] = {};
};

// A model of another level, from its scene file: an object's model, or a
// group of objects (a truck and its wheels) made one model when loaded, in
// the frame of pieces[0]: its invisible collision body (Chassis_coli) when
// it has one (whose collision the model gets), else its largest part.
struct LevelModel {
    uint32_t mesh = 0;       // its mesh reference; a group: 0x80000000 | its record
    std::string name;        // of the first object using it
    std::string group;       // that object's group (where it is in the level)
    uint32_t classId = 0;
    uint32_t flags = 0;      // node flags of that object
    float mat[9] = {};       // its rotation in the world
    float radius = 0;        // bounding sphere
    int uses = 0;
    int parts = 0;           // materials
    bool collision = false;  // it has collision data (record +0x30; a group: its largest part's)
    std::vector<LevelModelPart> pieces;  // a group's models (pieces[0]: the largest)
};
constexpr uint32_t kGroupModel = 0x80000000;
// The models of a level (cached; empty when it cannot be read). Only plain
// objects with a model of their own (no characters, no helpers).
const std::vector<LevelModel>& LevelModels(const std::string& level);

// A character of another level: its body (a skinned model) can be imported
// like a model and given to a character of this level with the same
// animations (see docs/models.md).
struct LevelCharacter {
    uint32_t mesh = 0;     // its body
    std::string model;     // "SoldierRussian_01"
    std::string animSet;   // "Reg_Male" (Worlds\Characters\_Anims\<set>)
    std::string group;     // the group of its first character
    int uses = 0;
    // its clothes on Hitman (HitmanAsGroup/<model>_HMas) and its body without
    // them (<model>_Nude): skinned models too, 0 when it has none
    uint32_t outfitMesh = 0, nudeMesh = 0;
    // its animation sets as its data block names them (the first two strings:
    // Worlds\Characters\_Anims\Reg_Male\Reg_Male.anm, an extra one or "")
    std::vector<std::string> animFiles;
    std::string behaviour;  // its first character's behaviour there ("BaseGuard", "Waiter"; "" none)
};
const std::vector<LevelCharacter>& LevelCharacters(const std::string& level);
// A model of the loaded scene made of plain records (not a character's body).
bool IsPlainModel(uint32_t mesh);
// The model's triangles stop nothing (every record's query mask, +0x34,
// cleared: the collision collectors skip them) or what they stopped before.
// Every object using the model is affected.
bool SetModelSolid(uint32_t mesh, bool solid);
// The animation sets of a body of another level loaded here (a character's,
// as its level names them; their animations come with it), empty for others.
std::vector<std::string> ImportedAnimFiles(uint32_t mesh);
// The animation set named in an actor's data block ("" when none).
std::string AnimSetOf(const uint8_t* block, size_t size);
// The animation sets of an actor's data block: its first two strings (the
// second one "" when it has no extra set; empty when it has no such strings).
std::vector<std::string> AnimFilesOf(const uint8_t* block, size_t size);
// The model name of an actor's data block (its first string value).
std::string ModelNameOf(const uint8_t* block, size_t size);
// Levels of the game ("C1-1", ...): folders of Scenes with a __MAIN.ZIP.
const std::vector<std::string>& GameLevels();

} // namespace h2::engine
