#pragma once
// The loaded scene's compiled records (.GMS), kept after loading.
//
// ZEngineDataBase::AllocSequence frees the GMS once CreateGeoms has built the
// objects, so the editor keeps a copy of it (CreateGeoms hook) and of the
// table "record index -> created object" the engine remaps references with
// (RemapCreatedDataBlock hook). With these, any record can be built again
// later (NPCs: data block and script), see re/npc.md.
#include <cstdint>
#include <string>
#include <vector>

#include "geoms.h"

namespace h2::engine {

struct SceneRecord {
    int index = 0;         // in the geom list (1-based in file references)
    std::string name;
    uint32_t classId = 0;
    uint32_t mesh = 0;
    uint32_t flags = 0;    // node flags (low 20 bits) and editor bits
    uint32_t extra = 0;
    uint32_t events = 0, props = 0, data = 0, script = 0;  // block offsets (0 = none)
    float mat[9] = {};     // parent space, as authored
    float pos[3] = {};
    int parent = -1;       // record of the enclosing group (-1: top level)
};

void InstallSceneData();

// Records of the scene loaded last (empty before a scene was loaded).
const std::vector<SceneRecord>& SceneRecords();
// The object the engine made of a record (null when gone or unknown).
ZGEOM* RecordGeom(int index);
// The record an object was made of (-1: none, e.g. made at runtime).
int RecordOf(const ZGEOM* geom);

// Copy of a typed data block of the GMS (data/script/event block at
// `offset`) with its references and message ids remapped to the runtime ones,
// marked as remapped. Empty when there is no such block.
std::vector<uint8_t> RemappedBlock(uint32_t offset);
// Copy of a block as stored in the file (references = 0x80000000 | record + 1).
std::vector<uint8_t> RawBlock(uint32_t offset);
// Offsets (in the block) of the reference values (tag 8) of a block.
std::vector<size_t> BlockRefOffsets(const std::vector<uint8_t>& block);

// ---- scripts (behaviour) ----

enum class ParamType : uint8_t { Double = 1, Float = 2, Long = 3, String = 4, Ref = 8, Other = 0 };

struct ScriptParam {
    std::string typeName;  // as declared: LONG, FLOAT, ZGEOMREF, ...
    std::string name;
    ParamType type = ParamType::Other;
    uint32_t bits = 0;     // Long / Float bits / Ref (file: 0x80000000|record, runtime: object ref)
    std::string text;      // String
    std::vector<uint8_t> raw;  // the value as stored (tag + payload), for types not edited
};

struct ScriptInfo {
    std::string name;      // e.g. "scripts\C1\C1_1\BaseGuard.sdl"
    std::vector<ScriptParam> params;
};

// Parses the script block at `offset` in the GMS (references as in the file).
bool ParseScript(uint32_t offset, ScriptInfo& out);
// Script block for a new object: references must already be runtime refs.
std::vector<uint8_t> BuildScriptBlock(const ScriptInfo& script);
// Runtime ref <-> object, file reference (0x80000000|record) -> runtime ref.
// The loaded scene's references can be used (false while scenes switch).
bool SceneDataValid();
uint32_t RefOf(ZGEOM* geom);
ZGEOM* GeomOfRef(uint32_t ref);
uint32_t RemapRef(uint32_t fileRef);
// "BaseGuard" for "scripts\C1\C1_1\BaseGuard.sdl"
std::string ScriptShortName(const std::string& path);

} // namespace h2::engine
