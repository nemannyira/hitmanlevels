// libFuzzer target for the level validator (docs/online_levels.md, section 11): any bytes as a
// level.json, both modes. Built only with -DH2_FUZZ=ON and clang-cl (-fsanitize=fuzzer,address).
#include <cstddef>
#include <cstdint>
#include <string>

#include "../../src/levelfmt/level_check.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    std::string text(reinterpret_cast<const char*>(data), size);
    h2::levelfmt::CheckLevelText(text, true).Summary();
    h2::levelfmt::CheckLevelText(text, false).Summary();
    return 0;
}
