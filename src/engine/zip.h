#pragma once
// Reading files out of the game's ZIP packages (Scenes\<L>\<L>__MAIN.ZIP):
// stored and deflated entries (own inflate, RFC 1951).
#include <cstdint>
#include <string>
#include <vector>

namespace h2::engine {

// The entry `name` (case-insensitive, '/' or '\') of the archive at `path`.
bool ReadZipEntry(const std::string& path, const std::string& name, std::vector<uint8_t>& out,
                  std::string* error = nullptr);
// Raw deflate data -> `out` (its size must be known: `size`).
bool Inflate(const uint8_t* data, size_t n, std::vector<uint8_t>& out, size_t size);

} // namespace h2::engine
