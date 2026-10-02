#pragma once
// Lists of objects (ZLIST, ZConditionList): routes, safe points, help points...
//
// A list keeps its elements in a reference container at +0x18 (chunked
// array, vtable 0x639510, `stride` dwords per element, the reference first).
// It is filled from the list's data block (ZLIST::ReadData 0x4437C0, for a
// ZConditionList 0x46CCB0, which also builds its per-element condition
// arrays). Elements are changed by clearing the container and reading a data
// block with the new reference array again, so a ZConditionList's own arrays
// stay consistent.
#include <cstdint>
#include <string>
#include <vector>

#include "geoms.h"

namespace h2::engine {

// A list whose elements the editor can change (ZLIST, ZConditionList).
bool IsEditableList(ZGEOM* geom);
// Its elements (references) in order, as the game sees them now.
std::vector<uint32_t> ListRefs(ZGEOM* list);
// Replaces its elements. Only lists of the scene file (their data block is the template).
bool SetListRefs(ZGEOM* list, const std::vector<uint32_t>& refs, std::string* error = nullptr);
// The elements the scene file gave it.
std::vector<uint32_t> AuthoredListRefs(ZGEOM* list);

} // namespace h2::engine
