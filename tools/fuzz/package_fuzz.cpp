// libFuzzer target for the package reader (src/levelfmt/package.h): any bytes as a .h2lvl file.
// Built only with -DH2_FUZZ=ON and clang-cl (-fsanitize=fuzzer,address).
#include <cstddef>
#include <cstdint>
#include <string>

#include "levelfmt/package.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    std::string bytes(reinterpret_cast<const char*>(data), size);
    h2::levelfmt::OpenedPackage p;
    std::string error;
    h2::levelfmt::OpenPackage(bytes, p, &error);
    return 0;
}
