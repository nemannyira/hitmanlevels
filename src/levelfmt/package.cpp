#include "package.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <set>

#include "crypto.h"
#include "json.hpp"
#include "level_check.h"

namespace h2::levelfmt {

using nlohmann::json;

namespace {

constexpr const char kSignedPrefix[] = "H2LVL manifest v1\n";
constexpr uint32_t kLocalSig = 0x04034b50, kCentralSig = 0x02014b50, kEndSig = 0x06054b50;
constexpr size_t kLocalSize = 30, kCentralSize = 46, kEndSize = 22;

bool Fail(std::string* error, const std::string& what)
{
    if (error)
        *error = what;
    return false;
}

uint32_t Crc32(const std::string& data)
{
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : data)
        c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint16_t U16(const std::string& s, size_t at) { return uint16_t(uint8_t(s[at]) | uint8_t(s[at + 1]) << 8); }
uint32_t U32(const std::string& s, size_t at) { return uint32_t(U16(s, at)) | uint32_t(U16(s, at + 2)) << 16; }
void Put16(std::string& s, uint16_t v) { s.push_back(char(v & 0xFF)), s.push_back(char(v >> 8)); }
void Put32(std::string& s, uint32_t v) { Put16(s, uint16_t(v & 0xFFFF)), Put16(s, uint16_t(v >> 16)); }

bool EqualNoCase(const std::string& a, const std::string& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// UTF-8 without overlong forms, surrogates or code points past U+10FFFF
bool ValidUtf8(const std::string& s)
{
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = s[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        int extra = c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : -1;
        if (extra < 0 || i + extra >= n)
            return false;
        uint32_t cp = c & (0x3F >> extra);
        for (int k = 1; k <= extra; ++k) {
            unsigned char cc = s[i + k];
            if ((cc & 0xC0) != 0x80)
                return false;
            cp = cp << 6 | (cc & 0x3F);
        }
        if ((extra == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ||
            (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF)))
            return false;
        i += extra + 1;
    }
    return true;
}

// text for people: UTF-8, no control characters but new lines / tabs where `lines`
bool PlainText(const std::string& s, bool lines)
{
    if (!ValidUtf8(s))
        return false;
    for (unsigned char c : s)
        if ((c < 0x20 && !(lines && (c == '\n' || c == '\r' || c == '\t'))) || c == 0x7F)
            return false;
    return true;
}

bool ScriptFileName(const std::string& name, std::string* script = nullptr)
{
    if (name.size() < 9 || name.compare(0, 8, "scripts/") != 0 || !ValidScriptName(name.substr(8)))
        return false;
    if (script)
        *script = name.substr(8);
    return true;
}

bool ValidPng(const std::string& d)
{
    static const char sig[] = "\x89PNG\r\n\x1a\n";
    if (d.size() < 33 || memcmp(d.data(), sig, 8) != 0 || memcmp(d.data() + 12, "IHDR", 4) != 0)
        return false;
    auto be = [&](size_t at) {
        return uint32_t(uint8_t(d[at])) << 24 | uint32_t(uint8_t(d[at + 1])) << 16 | uint32_t(uint8_t(d[at + 2])) << 8 |
               uint8_t(d[at + 3]);
    };
    uint32_t w = be(16), h = be(20);
    return be(8) == 13 && w > 0 && h > 0 && w <= uint32_t(kMaxPreviewSide) && h <= uint32_t(kMaxPreviewSide);
}

// a file's own limit and content check
bool CheckFile(const PackageFile& f, std::string* error)
{
    std::string script;
    if (f.name == "level.json") {
        if (f.data.size() > kMaxPackagedLevel)
            return Fail(error, "level.json is larger than " + std::to_string(kMaxPackagedLevel) + " bytes");
    } else if (ScriptFileName(f.name, &script)) {
        if (f.data.size() > kMaxPackagedScript)
            return Fail(error, f.name + " is larger than " + std::to_string(kMaxPackagedScript) + " bytes");
        if (!ValidUtf8(f.data) || f.data.find('\0') != std::string::npos)
            return Fail(error, f.name + " is not text (UTF-8)");
    } else if (f.name == "README.md") {
        if (f.data.size() > kMaxReadme)
            return Fail(error, "README.md is larger than " + std::to_string(kMaxReadme) + " bytes");
        if (!PlainText(f.data, true))
            return Fail(error, "README.md is not plain text (UTF-8)");
    } else if (f.name == "preview.png") {
        if (f.data.size() > kMaxPreview || !ValidPng(f.data))
            return Fail(error, "preview.png is not a PNG picture of at most " + std::to_string(kMaxPreviewSide) +
                                   " x " + std::to_string(kMaxPreviewSide) + " and " + std::to_string(kMaxPreview) +
                                   " bytes");
    } else {
        return Fail(error, "a file a package may not hold: " + f.name.substr(0, 64));
    }
    return true;
}

json ManifestJson(const Manifest& m)
{
    json files = json::array();
    for (const ManifestFile& f : m.files)
        files.push_back({{"name", f.name}, {"sha256", f.sha256}, {"size", f.size}});
    return {{"format", 1},
            {"id", m.id},
            {"version", m.version},
            {"title", m.title},
            {"base_scene", m.baseScene},
            {"author", {{"name", m.authorName}, {"key", m.authorKey}}},
            {"license", m.license},
            {"files", files}};
}

bool ParseManifest(const std::string& text, Manifest& m, std::string* error)
{
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception&) {
        return Fail(error, "manifest.json is not JSON");
    }
    // one form only (as BuildPackage writes it): no second key of a name, no other spacing or order
    if (!j.is_object() || j.dump() != text)
        return Fail(error, "manifest.json is not in its one canonical form");
    auto fields = [](const json& o, std::initializer_list<const char*> names) {
        if (!o.is_object() || o.size() != names.size())
            return false;
        for (const char* n : names)
            if (!o.contains(n))
                return false;
        return true;
    };
    if (!fields(j, {"format", "id", "version", "title", "base_scene", "author", "license", "files"}) ||
        !fields(j["author"], {"name", "key"}))
        return Fail(error, "manifest.json has other fields than a manifest");
    const json &format = j["format"], &version = j["version"];
    if (!format.is_number_integer() || format.get<long long>() != 1)
        return Fail(error, "a package format this editor does not know (a newer editor's?)");
    if (!version.is_number_integer() || version.get<long long>() < 1 || version.get<long long>() > kMaxPackageVersion)
        return Fail(error, "manifest.json: the version is not a whole number from 1");
    for (const char* key : {"id", "title", "base_scene", "license"})
        if (!j[key].is_string())
            return Fail(error, std::string("manifest.json: ") + key + " is not text");
    if (!j["author"]["name"].is_string() || !j["author"]["key"].is_string() || !j["files"].is_array())
        return Fail(error, "manifest.json: the author or the files are not what a manifest has");
    m.id = j["id"].get<std::string>();
    m.version = int(version.get<long long>());
    m.title = j["title"].get<std::string>();
    m.baseScene = j["base_scene"].get<std::string>();
    m.authorName = j["author"]["name"].get<std::string>();
    m.authorKey = j["author"]["key"].get<std::string>();
    m.license = j["license"].get<std::string>();
    uint8_t key[32];
    if (!ValidOnlineId(m.id))
        return Fail(error, "manifest.json: the id is not [a-z0-9]{8,32}");
    if (m.title.empty() || m.title.size() > 64 || !PlainText(m.title, false))
        return Fail(error, "manifest.json: the title is not a plain text of 1 to 64 bytes");
    if (m.authorName.empty() || m.authorName.size() > 40 || !PlainText(m.authorName, false))
        return Fail(error, "manifest.json: the author's name is not a plain text of 1 to 40 bytes");
    if (m.license.empty() || m.license.size() > 64 || !PlainText(m.license, false))
        return Fail(error, "manifest.json: the license is not a plain text of 1 to 64 bytes");
    if (m.baseScene.empty() || m.baseScene.size() > 128 || m.baseScene.find("..") != std::string::npos ||
        !std::all_of(m.baseScene.begin(), m.baseScene.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '\\';
        }))
        return Fail(error, "manifest.json: the base scene is not a scene's name");
    if (!crypto::FromHex(m.authorKey, key, 32))
        return Fail(error, "manifest.json: the author's key is not 64 lowercase hex digits");
    if (j["files"].size() > kMaxPackageFiles)
        return Fail(error, "manifest.json lists too many files");
    for (const json& f : j["files"]) {
        if (!fields(f, {"name", "sha256", "size"}) || !f["name"].is_string() || !f["sha256"].is_string() ||
            !f["size"].is_number_unsigned())
            return Fail(error, "manifest.json: a file entry is not a name, a size and a SHA-256");
        ManifestFile mf{f["name"].get<std::string>(), f["size"].get<size_t>(), f["sha256"].get<std::string>()};
        uint8_t digest[32];
        if (!AllowedPackageName(mf.name) || !crypto::FromHex(mf.sha256, digest, 32))
            return Fail(error, "manifest.json: a file entry with a name or a hash a package has not");
        m.files.push_back(mf);
    }
    return true;
}

std::string SignedMessage(const std::string& manifestText) { return kSignedPrefix + manifestText; }

void AddEntry(std::string& zip, std::string& central, const std::string& name, const std::string& data)
{
    uint32_t crc = Crc32(data), offset = uint32_t(zip.size()), size = uint32_t(data.size());
    // local header: version 2.0, no flags, stored, 1980-01-01 00:00 (a package does not tell when)
    Put32(zip, kLocalSig);
    Put16(zip, 20), Put16(zip, 0), Put16(zip, 0), Put16(zip, 0), Put16(zip, 0x21);
    Put32(zip, crc), Put32(zip, size), Put32(zip, size);
    Put16(zip, uint16_t(name.size())), Put16(zip, 0);
    zip += name;
    zip += data;
    Put32(central, kCentralSig);
    Put16(central, 20), Put16(central, 20), Put16(central, 0), Put16(central, 0), Put16(central, 0), Put16(central, 0x21);
    Put32(central, crc), Put32(central, size), Put32(central, size);
    Put16(central, uint16_t(name.size())), Put16(central, 0), Put16(central, 0), Put16(central, 0), Put16(central, 0);
    Put32(central, 0), Put32(central, offset);
    central += name;
}

// the archive's entries, in order, with every check of the layout
bool ReadZip(const std::string& z, std::vector<PackageFile>& out, std::string* error)
{
    if (z.size() > kMaxPackageBytes)
        return Fail(error, "the package is larger than " + std::to_string(kMaxPackageBytes >> 20) + " MB");
    // the end record last, with no comment: nothing may follow the files' directory
    if (z.size() < kEndSize || U32(z, z.size() - kEndSize) != kEndSig)
        return Fail(error, "not a level package (no ZIP end record at its end)");
    size_t end = z.size() - kEndSize;
    uint16_t disk = U16(z, end + 4), cdDisk = U16(z, end + 6), here = U16(z, end + 8), count = U16(z, end + 10);
    uint32_t cdSize = U32(z, end + 12), cdOffset = U32(z, end + 16);
    if (disk || cdDisk || here != count || U16(z, end + 20) != 0)
        return Fail(error, "the package's ZIP end record is not a plain one");
    if (count == 0 || count > kMaxPackageFiles + 2)
        return Fail(error, "the package holds " + std::to_string(count) + " files");
    if (cdOffset > end || cdSize != end - cdOffset)  // (no sums: size_t is 32 bits here)
        return Fail(error, "the package's ZIP directory is not right before its end record");
    size_t at = cdOffset, next = 0;  // the next entry's data must start where the one before ended
    for (uint16_t i = 0; i < count; ++i) {
        if (at + kCentralSize > end || U32(z, at) != kCentralSig)
            return Fail(error, "the package's ZIP directory is damaged");
        uint16_t needed = U16(z, at + 6), flags = U16(z, at + 8), method = U16(z, at + 10);
        uint32_t crc = U32(z, at + 16), packed = U32(z, at + 20), size = U32(z, at + 24);
        uint16_t nameLen = U16(z, at + 28), extraLen = U16(z, at + 30), commentLen = U16(z, at + 32);
        uint16_t diskStart = U16(z, at + 34);
        uint32_t external = U32(z, at + 38), offset = U32(z, at + 42);
        if (at + kCentralSize + nameLen + extraLen + commentLen > end)
            return Fail(error, "the package's ZIP directory is damaged");
        std::string name = z.substr(at + kCentralSize, nameLen);
        at += kCentralSize + nameLen + extraLen + commentLen;
        if (!AllowedPackageName(name))
            return Fail(error, "a file a package may not hold: \"" + name.substr(0, 64) + "\"");
        if (method != 0)
            return Fail(error, name + " is compressed: a level package holds its files stored (repacked by a ZIP tool?)");
        if (needed > 20 || (flags & ~0x0800) || diskStart || packed != size)
            return Fail(error, name + ": ZIP features a level package does not use (encryption, ZIP64, ...)");
        if ((external >> 16 & 0xF000) == 0xA000)
            return Fail(error, name + " is a link");
        if (offset != next)
            return Fail(error, "the package's files are not one after another (" + name + ")");
        if (size_t(offset) + kLocalSize > cdOffset || U32(z, offset) != kLocalSig)
            return Fail(error, name + ": its ZIP header is missing");
        uint16_t lNeeded = U16(z, offset + 4), lFlags = U16(z, offset + 6), lMethod = U16(z, offset + 8);
        uint32_t lCrc = U32(z, offset + 14), lPacked = U32(z, offset + 18), lSize = U32(z, offset + 22);
        uint16_t lNameLen = U16(z, offset + 26), lExtraLen = U16(z, offset + 28);
        size_t data = size_t(offset) + kLocalSize + lNameLen + lExtraLen;
        if (lNeeded > 20 || lFlags != flags || lMethod != 0 || lCrc != crc || lPacked != packed || lSize != size ||
            lNameLen != nameLen || data > cdOffset || z.compare(offset + kLocalSize, nameLen, name) != 0)
            return Fail(error, name + ": its two ZIP headers differ");
        if (size > cdOffset - data)
            return Fail(error, name + " goes past the package's files");
        PackageFile f{name, z.substr(data, size)};
        if (Crc32(f.data) != crc)
            return Fail(error, name + " is damaged (CRC)");
        next = uint32_t(data + size);
        for (const PackageFile& o : out)
            if (EqualNoCase(o.name, name))
                return Fail(error, "the package holds " + name + " twice");
        out.push_back(std::move(f));
    }
    if (at != end)
        return Fail(error, "the package's ZIP directory holds more than its files");
    if (next != cdOffset)
        return Fail(error, "the package holds data between its files and their directory");
    return true;
}

} // namespace

bool ValidManifestText(const std::string& s, size_t max) { return !s.empty() && s.size() <= max && PlainText(s, false); }

bool AllowedPackageName(const std::string& name)
{
    return name == "manifest.json" || name == "manifest.sig" || name == "level.json" || name == "README.md" ||
           name == "preview.png" || ScriptFileName(name);
}

const std::vector<std::string>& LicenseChoices()
{
    static const std::vector<std::string> choices = {"CC BY 4.0", "CC BY-NC 4.0", "CC0 1.0", "All rights reserved"};
    return choices;
}

std::string Fingerprint(const uint8_t pub[32])
{
    crypto::Sha256Digest d = crypto::Sha256(pub, 32);
    std::string hex = crypto::Hex(d.data(), 8), out;
    for (size_t i = 0; i < hex.size(); ++i) {
        if (i && i % 4 == 0)
            out += '-';
        out += char(std::toupper(static_cast<unsigned char>(hex[i])));
    }
    return out;
}

std::string Fingerprint(const std::string& keyHex)
{
    uint8_t pub[32];
    return crypto::FromHex(keyHex, pub, 32) ? Fingerprint(pub) : "?";
}

const PackageFile* OpenedPackage::Find(const std::string& name) const
{
    for (const PackageFile& f : files)
        if (f.name == name)
            return &f;
    return nullptr;
}

bool BuildPackage(Manifest m, const std::vector<PackageFile>& files, const AuthorKey& key, std::string& out,
                  std::string* error)
{
    m.authorKey = crypto::Hex(key.pub, 32);
    m.files.clear();
    std::vector<PackageFile> ordered = files;  // level.json first, then the scripts, README, preview
    auto rank = [](const std::string& n) { return n == "level.json" ? 0 : n == "README.md" ? 2 : n == "preview.png" ? 3 : 1; };
    std::stable_sort(ordered.begin(), ordered.end(), [&](const PackageFile& a, const PackageFile& b) {
        return rank(a.name) != rank(b.name) ? rank(a.name) < rank(b.name) : a.name < b.name;
    });
    for (const PackageFile& f : ordered) {
        if (f.name == "manifest.json" || f.name == "manifest.sig")
            return Fail(error, "a level's file named " + f.name);
        if (!CheckFile(f, error))
            return false;
        crypto::Sha256Digest d = crypto::Sha256(f.data.data(), f.data.size());
        m.files.push_back({f.name, f.data.size(), crypto::Hex(d.data(), d.size())});
    }
    std::string text = ManifestJson(m).dump();
    std::string message = SignedMessage(text);
    std::string sig(64, '\0');
    crypto::Ed25519Sign(reinterpret_cast<uint8_t*>(sig.data()), reinterpret_cast<const uint8_t*>(message.data()),
                        message.size(), key.seed, key.pub);
    std::string zip, central;
    AddEntry(zip, central, "manifest.json", text);
    AddEntry(zip, central, "manifest.sig", sig);
    for (const PackageFile& f : ordered)
        AddEntry(zip, central, f.name, f.data);
    uint32_t cdOffset = uint32_t(zip.size());
    zip += central;
    uint16_t count = uint16_t(ordered.size() + 2);
    Put32(zip, kEndSig);
    Put16(zip, 0), Put16(zip, 0), Put16(zip, count), Put16(zip, count);
    Put32(zip, uint32_t(central.size())), Put32(zip, cdOffset), Put16(zip, 0);
    // what installs, as it is checked when it installs
    OpenedPackage check;
    if (!OpenPackage(zip, check, error))
        return false;
    out = std::move(zip);
    return true;
}

bool OpenPackage(const std::string& bytes, OpenedPackage& out, std::string* error)
{
    out = {};
    std::vector<PackageFile> entries;
    if (!ReadZip(bytes, entries, error))
        return false;
    if (entries.size() < 3 || entries[0].name != "manifest.json" || entries[1].name != "manifest.sig")
        return Fail(error, "the package does not start with its manifest and signature");
    const std::string& text = entries[0].data;
    const std::string& sig = entries[1].data;
    if (text.size() > kMaxManifestBytes)
        return Fail(error, "manifest.json is larger than " + std::to_string(kMaxManifestBytes) + " bytes");
    if (sig.size() != 64)
        return Fail(error, "manifest.sig is not a signature (64 bytes)");
    Manifest m;
    if (!ParseManifest(text, m, error))
        return false;
    // the signature before anything else of the package is believed
    uint8_t pub[32];
    crypto::FromHex(m.authorKey, pub, 32);
    std::string message = SignedMessage(text);
    if (!crypto::Ed25519Verify(reinterpret_cast<const uint8_t*>(sig.data()),
                               reinterpret_cast<const uint8_t*>(message.data()), message.size(), pub))
        return Fail(error, "the signature does not match: the package was changed after its author signed it");
    // exactly the files the manifest lists, as it lists them
    if (m.files.size() != entries.size() - 2)
        return Fail(error, "the package holds other files than its manifest lists");
    for (size_t i = 2; i < entries.size(); ++i) {
        const PackageFile& f = entries[i];
        const ManifestFile& mf = m.files[i - 2];
        if (f.name != mf.name)
            return Fail(error, "the package holds other files than its manifest lists (" + f.name + ")");
        crypto::Sha256Digest d = crypto::Sha256(f.data.data(), f.data.size());
        if (f.data.size() != mf.size || crypto::Hex(d.data(), d.size()) != mf.sha256)
            return Fail(error, f.name + " is not the file its author signed (size / SHA-256)");
        if (!CheckFile(f, error))
            return false;
    }
    if (m.files.empty() || m.files[0].name != "level.json")
        return Fail(error, "the package has no level.json");
    // the level as a downloaded level is loaded (strict), and as the manifest describes it
    const std::string& levelText = entries[2].data;
    Report report = CheckLevelText(levelText, true);
    if (!report.Ok())
        return Fail(error, "level.json is refused:\n" + report.Summary());
    json level = json::parse(levelText);
    if (!level.contains("package") || level["package"].value("id", "") != m.id ||
        level["package"].value("version", 0) != m.version)
        return Fail(error, "level.json is not the level of this package (its package id / version)");
    if (level.value("base_scene", "") != m.baseScene || level.value("name", "") != m.title)
        return Fail(error, "level.json's name or base scene is not the manifest's");
    std::set<std::string> listed, packed;
    for (const json& s : level.value("scripts", json::array()))
        listed.insert(s.get<std::string>());
    for (size_t i = 3; i < entries.size(); ++i) {
        std::string script;
        if (ScriptFileName(entries[i].name, &script))
            packed.insert(script);
    }
    if (listed != packed)
        return Fail(error, "the package's scripts are not the ones its level.json lists");
    out.manifest = std::move(m);
    out.manifestText = text;
    out.files.assign(entries.begin() + 2, entries.end());
    return true;
}

} // namespace h2::levelfmt
