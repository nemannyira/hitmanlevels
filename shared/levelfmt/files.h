#pragma once
// The file and key helpers share.cpp and hub.cpp both use (levelfmt's own, not for the DLL's other code):
// paths of the game's side in the system's code page, the data folder (%LOCALAPPDATA%\HitmanLevels or
// HITMANLEVELS_HOME), whole-file reads / atomic writes, DPAPI for this Windows user.
#include <string>

#include "json.hpp"

namespace h2::levelfmt::files {

bool Fail(std::string* error, const std::string& what);  // false, with `what` as the error
std::wstring GamePath(const std::string& acp);
std::string Utf8(const std::wstring& w);
std::wstring DataDir(bool make = false);  // with a trailing backslash; made only by what writes there
bool ReadFileW(const std::wstring& path, std::string& out, size_t max);
bool ReadFileA(const std::string& path, std::string& out, size_t max);  // (a game-side path)
bool WriteNewFileW(const std::wstring& path, const std::string& data, bool replace);
bool WriteFileAtomic(const std::wstring& path, const std::string& data);
bool Protect(const std::string& plain, std::string& out);    // DPAPI, this Windows user
bool Unprotect(const std::string& sealed, std::string& out);
nlohmann::json ReadJsonW(const std::wstring& path, std::string* why = nullptr);

} // namespace h2::levelfmt::files
