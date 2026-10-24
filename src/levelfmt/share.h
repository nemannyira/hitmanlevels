#pragma once
// Passing levels on as packages (package.h) on this PC: the author's key, making a level folder's
// package, and installing one into Mods\Online\<id> with what this PC knows of its authors.
//
// Kept in %LOCALAPPDATA%\H2LevelEditor (H2LEVEL_HOME instead, for tests):
//   author.json   the author's name, license and public key
//   author.key    the key's secret seed, encrypted for this Windows user (DPAPI); it never leaves the PC
//   known.json    the authors this PC accepted, and each installed level's key and highest version
#include <string>
#include <vector>

#include "json.hpp"
#include "package.h"

namespace h2::levelfmt {

std::string ShareDataDir();  // with a trailing backslash (made when the author key or known.json is written)

struct AuthorInfo {
    std::string name;
    std::string license;
    std::string keyHex;
    std::string Fingerprint() const { return levelfmt::Fingerprint(keyHex); }
};

// The author of this PC (false: none made yet, or the key cannot be read by this Windows user).
bool LoadAuthor(AuthorInfo& info, AuthorKey* key = nullptr, std::string* error = nullptr);
// Makes the author's key once (a second call keeps the key, changes the name / license).
bool SetAuthor(const std::string& name, const std::string& license, AuthorInfo* info, std::string* error);

// Gives a level file its package id (when it has none) and the next version: `level` is the parsed
// level.json, changed in place. False when the level cannot have one.
bool StampPackage(nlohmann::json& level, std::string* error);

// The package of the level in `folder` (its level.json, the scripts it lists, README.md and preview.png
// when there): checked as an installer checks it, signed with the author's key. The level.json must
// carry its package stamp (StampPackage). Refused when a file holds a path of this PC (C:\Users\...).
bool ExportLevelFolder(const std::string& folder, std::string& package, Manifest* manifest, std::string* error);

// What this PC knows of a package's author and level.
struct TrustCheck {
    enum Kind {
        NewAuthor,     // an author not accepted on this PC before
        KnownAuthor,   // accepted before; the level new or a newer version of it
        OtherAuthor,   // the level is installed, but another key signed it: someone else's level of that id?
        Older,         // an older version than the highest one installed before
        SameVersion,   // this very package is installed (its manifest in the level's folder)
    } kind = NewAuthor;
    std::string knownName;  // the name the key had when it was accepted (it can differ from the manifest's)
    std::string installedKey;
    int installedVersion = 0;  // installed now
    int highestVersion = 0;    // installed ever (an older one only when asked)
};
TrustCheck CheckTrust(const OpenedPackage& p, const std::string& gameDir);

// Installs an opened (checked) package into <gameDir>\Mods\Online\<id> (a new folder made first, then
// put in the old one's place; the files read-only) and remembers its author and version.
bool InstallPackage(const OpenedPackage& p, const std::string& gameDir, std::string* error);

} // namespace h2::levelfmt
