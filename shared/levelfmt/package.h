#pragma once
// A level package (.h2lvl, docs/online_levels.md section 3): a ZIP of stored (not compressed) files
//
//   manifest.json   the level's id, version, title, base scene, author (name, public key), license and
//                   every other file's size and SHA-256, in one canonical form (compact, sorted keys)
//   manifest.sig    the author's Ed25519 signature of "H2LVL manifest v1\n" + manifest.json (64 bytes)
//   level.json      the level
//   scripts/*.lua   its scripts (the names its level.json lists)
//   README.md       optional, shown by the installer only
//   preview.png     optional, shown by the installer only
//
// OpenPackage makes every check before anything of it is used: the archive's own layout (no
// compression, so no bombs; no other names; nothing between or around the files), the signature, the
// hashes, the level (CheckLevel strict) and the scripts' text. Names from the archive are never paths:
// each allowed name has its place in the install folder (share.h).
#include <cstdint>
#include <string>
#include <vector>

namespace h2::levelfmt {

constexpr size_t kMaxPackageBytes = 2u << 20;  // the .h2lvl file
constexpr size_t kMaxPackageFiles = 2 + 1 + 16 + 2;  // manifest.*, level.json, scripts (kMaxScripts), README, preview
constexpr size_t kMaxManifestBytes = 64u << 10;
constexpr size_t kMaxPackagedLevel = 1u << 20;  // level.json in a package
constexpr size_t kMaxPackagedScript = 256u << 10;
constexpr size_t kMaxReadme = 64u << 10;
constexpr size_t kMaxPreview = 1u << 20;
constexpr int kMaxPreviewSide = 2048;
constexpr int kMaxPackageVersion = 0x7FFFFFFF;

struct PackageFile {
    std::string name;  // its name in the package ("level.json", "scripts/main.lua")
    std::string data;
};

struct ManifestFile {
    std::string name;
    size_t size = 0;
    std::string sha256;  // lowercase hex
};

struct Manifest {
    std::string game;  // the game whose level it is ("h2"; shared/levelfmt/games.h)
    std::string id;  // [a-z0-9]{8,32}: its folder <game>\Mods\Online\<id> when installed
    int version = 0;  // grows with every export
    std::string title;
    std::string baseScene;
    std::string authorName;
    std::string authorKey;  // the Ed25519 public key, 64 lowercase hex
    std::string license;
    std::vector<ManifestFile> files;  // every file but manifest.json / manifest.sig
};

struct AuthorKey {
    uint8_t seed[32] = {};  // secret
    uint8_t pub[32] = {};
};

// Text for people in a manifest (title, author, license): UTF-8, no control characters, 1 to `max` bytes.
bool ValidManifestText(const std::string& s, size_t max);
// The PNG with the picture's own chunks only (no text, EXIF, times: what can hold a name or a path); ""
// when it is not a PNG a package may hold (at most kMaxPreviewSide square, CRCs right).
std::string CleanPng(const std::string& png);
// A name a package may hold.
bool AllowedPackageName(const std::string& name);
// "photo1.png" .. "photo5.png": a picture of the laptop's files of the level (level.json mission.files), held
// and checked as preview.png is (a PNG of its picture's chunks only, at most kMaxPreviewSide square).
bool PhotoFileName(const std::string& name);
// The licenses the editor offers (the first one its default); a manifest may name another (text).
const std::vector<std::string>& LicenseChoices();
// Who signed: 16 hex digits of the key's SHA-256 in groups of four ("3F2A-91C0-77AB-0D12").
std::string Fingerprint(const std::string& keyHex);
std::string Fingerprint(const uint8_t pub[32]);

// The package of these files (level.json, scripts/<name>, README.md, preview.png) signed with `key`;
// `m.files` is filled from them, `m.authorKey` from the key.
bool BuildPackage(Manifest m, const std::vector<PackageFile>& files, const AuthorKey& key, std::string& out,
                  std::string* error);

struct OpenedPackage {
    Manifest manifest;
    std::string manifestText;  // as signed
    std::vector<PackageFile> files;  // every file but manifest.json / manifest.sig
    const PackageFile* Find(const std::string& name) const;
};

// Every check (see above); false with the first reason otherwise.
bool OpenPackage(const std::string& bytes, OpenedPackage& out, std::string* error);

} // namespace h2::levelfmt
