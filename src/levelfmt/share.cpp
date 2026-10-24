#include "share.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <dpapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "crypto.h"
#include "level_check.h"

namespace h2::levelfmt {

using nlohmann::json;

namespace {

bool Fail(std::string* error, const std::string& what)
{
    if (error)
        *error = what;
    return false;
}

// a path of the game's side (the DLL's and the command line's are in the system's code page)
std::wstring GamePath(const std::string& acp)
{
    int n = MultiByteToWideChar(CP_ACP, 0, acp.data(), int(acp.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, acp.data(), int(acp.size()), w.data(), n);
    return w;
}

bool Exists(const std::string& path) { return GetFileAttributesW(GamePath(path).c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string Utf8(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring DataDir(bool make = false)  // (made only by what writes there)
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"H2LEVEL_HOME", buf, MAX_PATH);
    std::wstring dir;
    if (n && n < MAX_PATH) {
        dir = buf;
    } else {
        PWSTR local = nullptr;  // the PC's own (not roaming): the key stays here
        if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local) == S_OK)
            dir = std::wstring(local) + L"\\H2LevelEditor";
        CoTaskMemFree(local);
    }
    if (dir.empty())
        return dir;
    if (make)
        CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\";
}

// a file of at most `max` bytes, whole
bool ReadFileW(const std::wstring& path, std::string& out, size_t max)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart <= LONGLONG(max);
    if (ok) {
        out.resize(size_t(size.QuadPart));
        DWORD got = 0;
        ok = out.empty() || (ReadFile(h, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size());
    }
    CloseHandle(h);
    return ok;
}

bool WriteNewFileW(const std::wstring& path, const std::string& data, bool replace)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, replace ? CREATE_ALWAYS : CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD put = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), DWORD(data.size()), &put, nullptr) && put == data.size());
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (!ok)
        DeleteFileW(path.c_str());
    return ok;
}

// written beside, then put in its place: a crash leaves the old file or the new one, never half of one
bool WriteFileAtomic(const std::wstring& path, const std::string& data)
{
    std::wstring tmp = path + L".tmp";
    return WriteNewFileW(tmp, data, true) &&
           MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

bool ReadFileA(const std::string& path, std::string& out, size_t max) { return ReadFileW(GamePath(path), out, max); }

const char kKeyEntropy[] = "H2LVL author key v1";

bool Protect(const std::string& plain, std::string& out)
{
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))}, blob{};
    DATA_BLOB entropy{DWORD(sizeof kKeyEntropy - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kKeyEntropy))};
    if (!CryptProtectData(&in, L"H2 level editor author key", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                          &blob))
        return false;
    out.assign(reinterpret_cast<char*>(blob.pbData), blob.cbData);
    LocalFree(blob.pbData);
    return true;
}

bool Unprotect(const std::string& sealed, std::string& out)
{
    DATA_BLOB in{DWORD(sealed.size()), reinterpret_cast<BYTE*>(const_cast<char*>(sealed.data()))}, blob{};
    DATA_BLOB entropy{DWORD(sizeof kKeyEntropy - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kKeyEntropy))};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &blob))
        return false;
    out.assign(reinterpret_cast<char*>(blob.pbData), blob.cbData);
    SecureZeroMemory(blob.pbData, blob.cbData);
    LocalFree(blob.pbData);
    return true;
}

json ReadJsonW(const std::wstring& path)
{
    std::string text;
    if (!ReadFileW(path, text, 4u << 20))
        return json();
    try {
        return json::parse(text);
    } catch (const std::exception&) {
        return json();
    }
}

std::string RandomHex(size_t bytes)
{
    std::string raw(bytes, '\0');
    if (!crypto::RandomBytes(raw.data(), raw.size()))
        return "";
    return crypto::Hex(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
}

bool IsReparse(const std::string& path)
{
    DWORD a = GetFileAttributesW(GamePath(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

// a folder this installer made: its files (read-only ones too), then the folder
void RemoveInstalled(const std::string& dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(GamePath(dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
                continue;  // (none of ours: left)
            std::wstring f = GamePath(dir) + L"\\" + fd.cFileName;
            SetFileAttributesW(f.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(f.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(GamePath(dir).c_str());
}

// a path of this PC in a file meant for others ("C:\Users\<name>\..."): the user's name would go with it
bool HoldsPrivatePath(const std::string& text)
{
    std::string low(text);
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* p : {":\\users\\", ":\\\\users\\\\", ":/users/", "\\documents and settings\\",
                          "\\\\documents and settings\\\\"})
        if (low.find(p) != std::string::npos)
            return true;
    wchar_t profile[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (n >= 4 && n < MAX_PATH) {
        std::string p = Utf8(profile);
        std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (low.find(p) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

std::string ShareDataDir() { return Utf8(DataDir()); }

bool LoadAuthor(AuthorInfo& info, AuthorKey* key, std::string* error)
{
    std::wstring dir = DataDir();
    json j = ReadJsonW(dir + L"author.json");
    if (!j.is_object() || !j.value("name", json()).is_string() || !j.value("key", json()).is_string())
        return Fail(error, "no author yet (" + Utf8(dir) + "author.json): give your name once to make your author key");
    info.name = j["name"].get<std::string>();
    info.license = j.value("license", json()).is_string() ? j["license"].get<std::string>() : LicenseChoices()[0];
    info.keyHex = j["key"].get<std::string>();
    if (!ValidManifestText(info.name, 40) || !ValidManifestText(info.license, 64))
        return Fail(error, "author.json is damaged (the name or the license)");
    if (!key)
        return true;
    std::string sealed, seed;
    if (!ReadFileW(dir + L"author.key", sealed, 4096) || !Unprotect(sealed, seed) || seed.size() != 32)
        return Fail(error, "the author key cannot be read (made by another Windows user, or damaged)");
    memcpy(key->seed, seed.data(), 32);
    SecureZeroMemory(seed.data(), seed.size());
    crypto::Ed25519PublicKey(key->seed, key->pub);
    if (crypto::Hex(key->pub, 32) != info.keyHex)
        return Fail(error, "the author key does not belong to author.json");
    return true;
}

bool SetAuthor(const std::string& name, const std::string& license, AuthorInfo* info, std::string* error)
{
    if (!ValidManifestText(name, 40))
        return Fail(error, "the name: 1 to 40 letters, no control characters");
    if (!ValidManifestText(license, 64))
        return Fail(error, "the license: 1 to 64 letters");
    std::wstring dir = DataDir(true);
    if (dir.empty())
        return Fail(error, "no folder for the author key (LocalAppData)");
    AuthorInfo old;
    AuthorKey key;
    std::string keyError;
    bool have = LoadAuthor(old, &key, &keyError);
    if (!have && GetFileAttributesW((dir + L"author.key").c_str()) != INVALID_FILE_ATTRIBUTES)
        return Fail(error, keyError + " - not replaced: move author.key away in " + Utf8(dir) + " to make a new one");
    if (!have) {
        if (!crypto::RandomBytes(key.seed, 32))
            return Fail(error, "no random bytes from the system");
        crypto::Ed25519PublicKey(key.seed, key.pub);
        std::string sealed;
        if (!Protect(std::string(reinterpret_cast<char*>(key.seed), 32), sealed) ||
            !WriteNewFileW(dir + L"author.key", sealed, false))
            return Fail(error, "the author key cannot be written to " + Utf8(dir));
    }
    AuthorInfo now{name, license, crypto::Hex(key.pub, 32)};
    SecureZeroMemory(key.seed, sizeof key.seed);
    json j = {{"name", now.name}, {"license", now.license}, {"key", now.keyHex}};
    if (!WriteFileAtomic(dir + L"author.json", j.dump(2)))
        return Fail(error, "author.json cannot be written to " + Utf8(dir));
    if (info)
        *info = now;
    return true;
}

bool StampPackage(json& level, std::string* error)
{
    if (!level.is_object())
        return Fail(error, "not a level");
    json& p = level["package"];
    if (!p.is_object() || !p.value("id", json()).is_string() || !ValidOnlineId(p["id"].get<std::string>())) {
        // a new id: 16 of [a-z0-9] (bytes past 251 drawn again: every letter as likely)
        std::string id;
        while (id.size() < 16) {
            uint8_t b[32];
            if (!crypto::RandomBytes(b, sizeof b))
                return Fail(error, "no random bytes from the system");
            for (uint8_t v : b)
                if (v < 252 && id.size() < 16)
                    id += "abcdefghijklmnopqrstuvwxyz0123456789"[v % 36];
        }
        p = {{"id", id}, {"version", 0}};
    }
    long long v = p.value("version", json()).is_number_integer() ? p["version"].get<long long>() : 0;
    if (v < 0 || v >= kMaxPackageVersion)
        return Fail(error, "the level's package version is out of range");
    p["version"] = v + 1;
    return true;
}

bool ExportLevelFolder(const std::string& folder, std::string& package, Manifest* manifest, std::string* error)
{
    std::vector<PackageFile> files;
    PackageFile level{"level.json"};
    if (!ReadFileA(folder + "\\level.json", level.data, kMaxPackagedLevel))
        return Fail(error, "level.json is missing or larger than " + std::to_string(kMaxPackagedLevel >> 10) + " KB");
    json j;
    try {
        j = json::parse(level.data);
    } catch (const std::exception&) {
        return Fail(error, "level.json is not JSON");
    }
    Report report = CheckLevel(j, true);  // what an installer would make of it
    if (!report.Ok())
        return Fail(error, "the level would be refused where it is installed:\n" + report.Summary());
    if (!j.contains("package"))
        return Fail(error, "the level has no package id yet (StampPackage)");
    files.push_back(level);
    for (const json& s : j.value("scripts", json::array())) {
        std::string name = s.get<std::string>();
        PackageFile f{"scripts/" + name};
        if (!ReadFileA(folder + "\\" + name, f.data, kMaxPackagedScript))
            return Fail(error, "the script " + name + " the level lists is missing or larger than " +
                                   std::to_string(kMaxPackagedScript >> 10) + " KB");
        files.push_back(std::move(f));
    }
    for (auto [name, max] : {std::pair<const char*, size_t>{"README.md", kMaxReadme}, {"preview.png", kMaxPreview}}) {
        PackageFile f{name};
        if (!Exists(folder + "\\" + name))
            continue;
        if (!ReadFileA(folder + "\\" + name, f.data, max))
            return Fail(error, std::string(name) + " is larger than " + std::to_string(max >> 10) + " KB");
        files.push_back(std::move(f));
    }
    for (const PackageFile& f : files)
        if (f.name != "preview.png" && HoldsPrivatePath(f.data))
            return Fail(error, f.name + " holds a path of this PC (C:\\Users\\...: your Windows user name would go "
                                        "with the package); take it out first");
    AuthorInfo author;
    AuthorKey key;
    if (!LoadAuthor(author, &key, error))
        return false;
    Manifest m;
    m.id = j["package"]["id"].get<std::string>();
    m.version = j["package"]["version"].get<int>();
    m.title = j.value("name", "");
    m.baseScene = j.value("base_scene", "");
    m.authorName = author.name;
    m.license = author.license;
    if (!ValidManifestText(m.title, 64))
        return Fail(error, "the level has no name");
    bool ok = BuildPackage(m, files, key, package, error);
    SecureZeroMemory(key.seed, sizeof key.seed);
    if (ok && manifest) {
        OpenedPackage p;
        OpenPackage(package, p, nullptr);
        *manifest = p.manifest;
    }
    return ok;
}

TrustCheck CheckTrust(const OpenedPackage& p, const std::string& gameDir)
{
    TrustCheck t;
    json known = ReadJsonW(DataDir() + L"known.json");
    const Manifest& m = p.manifest;
    if (known.is_object() && known.value("authors", json()).is_object() && known["authors"].contains(m.authorKey) &&
        known["authors"][m.authorKey].is_object()) {
        t.kind = TrustCheck::KnownAuthor;
        t.knownName = known["authors"][m.authorKey].value("name", "");
    }
    if (known.is_object() && known.value("levels", json()).is_object() && known["levels"].contains(m.id) &&
        known["levels"][m.id].is_object()) {
        const json& l = known["levels"][m.id];
        t.installedKey = l.value("key", "");
        t.installedVersion = l.value("installed", 0);
        t.highestVersion = l.value("highest", 0);
        std::string installed;
        if (t.installedKey != m.authorKey)
            t.kind = TrustCheck::OtherAuthor;
        else if (ReadFileA(gameDir + "\\Mods\\Online\\" + m.id + "\\package.json", installed, kMaxManifestBytes) &&
                 installed == p.manifestText)
            t.kind = TrustCheck::SameVersion;
        else if (m.version < t.highestVersion)
            t.kind = TrustCheck::Older;
    }
    return t;
}

bool InstallPackage(const OpenedPackage& p, const std::string& gameDir, std::string* error)
{
    const Manifest& m = p.manifest;
    if (!ValidOnlineId(m.id))  // (OpenPackage checked it; the folder's name comes from it)
        return Fail(error, "not an opened package");
    if (GetFileAttributesW(GamePath(gameDir + "\\hitman2.exe").c_str()) == INVALID_FILE_ATTRIBUTES)
        return Fail(error, gameDir + " is not the game's folder (no hitman2.exe)");
    std::string mods = gameDir + "\\Mods", online = mods + "\\Online";
    CreateDirectoryW(GamePath(mods).c_str(), nullptr);
    CreateDirectoryW(GamePath(online).c_str(), nullptr);
    if (IsReparse(mods) || IsReparse(online))
        return Fail(error, "Mods\\Online is a link: not installed into it");
    std::string tmp = online + "\\.new-" + RandomHex(8), target = online + "\\" + m.id;
    if (tmp.size() == online.size() + 6 || !CreateDirectoryW(GamePath(tmp).c_str(), nullptr))
        return Fail(error, "cannot make a folder in " + online);
    // each allowed name to its fixed place; nothing of the archive's names becomes a path otherwise
    auto put = [&](const std::string& name, const std::string& data) {
        std::wstring path = GamePath(tmp + "\\" + name);
        return WriteNewFileW(path, data, false) && SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY);
    };
    bool ok = put("package.json", p.manifestText);
    for (const PackageFile& f : p.files) {
        if (!ok)
            break;
        std::string name = f.name.rfind("scripts/", 0) == 0 ? f.name.substr(8) : f.name;
        if (name != "level.json" && name != "README.md" && name != "preview.png" && !ValidScriptName(name))
            ok = false;
        else
            ok = put(name, f.data);
    }
    if (!ok) {
        RemoveInstalled(tmp);
        return Fail(error, "cannot write the level's files into " + online);
    }
    std::string old;
    if (GetFileAttributesW(GamePath(target).c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (IsReparse(target)) {
            RemoveInstalled(tmp);
            return Fail(error, target + " is a link: not replaced");
        }
        old = online + "\\.old-" + RandomHex(8);
        if (!MoveFileW(GamePath(target).c_str(), GamePath(old).c_str())) {
            RemoveInstalled(tmp);
            return Fail(error, "the installed version cannot be replaced (is the level open in the game?)");
        }
    }
    if (!MoveFileW(GamePath(tmp).c_str(), GamePath(target).c_str())) {
        if (!old.empty())
            MoveFileW(GamePath(old).c_str(), GamePath(target).c_str());
        RemoveInstalled(tmp);
        return Fail(error, "cannot put the level in place: " + target);
    }
    if (!old.empty())
        RemoveInstalled(old);
    // what this PC knows now: the author accepted, the level's key and highest version
    std::wstring knownPath = DataDir(true) + L"known.json";
    json known = ReadJsonW(knownPath);
    if (!known.is_object())
        known = json::object();
    if (!known.value("authors", json()).is_object())
        known["authors"] = json::object();
    if (!known.value("levels", json()).is_object())
        known["levels"] = json::object();
    if (!known["authors"].contains(m.authorKey))
        known["authors"][m.authorKey] = {{"name", m.authorName}};
    int highest = known["levels"].contains(m.id) && known["levels"][m.id].is_object() &&
                          known["levels"][m.id].value("key", "") == m.authorKey
                      ? known["levels"][m.id].value("highest", 0)
                      : 0;
    known["levels"][m.id] = {
        {"key", m.authorKey}, {"installed", m.version}, {"highest", std::max(highest, m.version)}, {"title", m.title}};
    if (!WriteFileAtomic(knownPath, known.dump(2)))
        return Fail(error, "installed, but known.json cannot be written in " + ShareDataDir());
    return true;
}

} // namespace h2::levelfmt
