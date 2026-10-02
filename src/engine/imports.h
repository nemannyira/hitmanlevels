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
// same name).
#include <cstdint>
#include <string>
#include <vector>

namespace h2::engine {

struct ModelImport {
    std::string level;  // e.g. "C2-1"
    uint32_t mesh = 0;  // mesh reference in that level (a record of its PRM)
};

void InstallImports();
// Models made available in the scenes loaded from now on (all scenes but the
// menus), replacing the earlier list.
void SetModelImports(const std::vector<ModelImport>& list);
// The mesh reference of an imported model in the loaded scene (0: not there).
uint32_t ImportedMesh(const std::string& level, uint32_t mesh);

} // namespace h2::engine
