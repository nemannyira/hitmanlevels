#include "share.h"
#include "files.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <dpapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <new>
#include <vector>

#include "crypto.h"
#include "games.h"
#include "names.h"
#include "monocypher.h"

namespace h2::levelfmt {

using nlohmann::json;

namespace files {

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

std::wstring DataDir(bool make)  // (made only by what writes there)
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"HITMANLEVELS_HOME", buf, MAX_PATH);
    std::wstring dir;
    if (n && n < MAX_PATH) {
        dir = buf;
    } else {
        PWSTR local = nullptr;  // the PC's own (not roaming): the key stays here
        if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local) == S_OK) {
            dir = std::wstring(local) + L"\\HitmanLevels";
            // the folder's name before the games shared it: an author key made then moves along
            std::wstring old = std::wstring(local) + L"\\H2LevelEditor";
            if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesW((old + L"\\author.key").c_str()) != INVALID_FILE_ATTRIBUTES)
                MoveFileW(old.c_str(), dir.c_str());
        }
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
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
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

json ReadJsonW(const std::wstring& path, std::string* why)
{
    std::string text;
    if (!ReadFileW(path, text, 4u << 20)) {
        if (why)
            *why = "cannot read it: error " + std::to_string(GetLastError());
        return json();
    }
    try {
        return json::parse(text);
    } catch (const std::exception& ex) {
        if (why)
            *why = std::string("not JSON: ") + ex.what();
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

std::string WindowsUserName()
{
    wchar_t name[256];
    DWORD n = GetEnvironmentVariableW(L"USERNAME", name, 256);
    return n >= 3 && n < 256 ? Utf8(std::wstring(name, n)) : "";
}

// `word` in the text, not part of a longer word (case-insensitive, ASCII letters)
bool HoldsWord(const std::string& text, const std::string& word)
{
    if (word.empty())
        return false;
    auto low = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return s;
    };
    std::string t = low(text), w = low(word);
    auto letter = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80; };
    for (size_t at = t.find(w); at != std::string::npos; at = t.find(w, at + 1))
        if ((at == 0 || !letter(t[at - 1])) && (at + w.size() == t.size() || !letter(t[at + w.size()])))
            return true;
    return false;
}

} // namespace files

using namespace files;

std::string ShareDataDir() { return Utf8(DataDir()); }

bool LoadAuthor(AuthorInfo& info, AuthorKey* key, std::string* error)
{
    std::wstring dir = DataDir();
    std::string why = "no name or key in it";
    json j = ReadJsonW(dir + L"author.json", &why);
    if (!j.is_object() || !j.value("name", json()).is_string() || !j.value("key", json()).is_string())
        return Fail(error, "no author yet (" + Utf8(dir) + "author.json: " + why + "): give your name once to make your author key");
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

namespace {

constexpr char kBackupMagic[] = "H2LVL key backup 1\n";
constexpr size_t kBackupMagicSize = sizeof kBackupMagic - 1;
constexpr size_t kBackupHeader = kBackupMagicSize + 8 + 16 + 24;  // magic, blocks, passes, salt, nonce
constexpr uint32_t kBackupBlocks = 32768, kBackupPasses = 3;  // 32 MB of Argon2id

// the key a password and salt make (false: no memory for Argon2's work area)
bool PasswordKey(const std::string& password, const uint8_t salt[16], uint32_t blocks, uint32_t passes, uint8_t key[32])
{
    std::vector<uint8_t> work;
    try {
        work.resize(size_t(blocks) * 1024);
    } catch (const std::bad_alloc&) {
        return false;
    }
    crypto_argon2_config config{CRYPTO_ARGON2_ID, blocks, passes, 1};
    crypto_argon2_inputs inputs{reinterpret_cast<const uint8_t*>(password.data()), salt, uint32_t(password.size()), 16};
    crypto_argon2(key, 32, work.data(), config, inputs, crypto_argon2_no_extras);
    crypto_wipe(work.data(), work.size());
    return true;
}

void Put32(std::string& s, size_t at, uint32_t v) { memcpy(&s[at], &v, 4); }
uint32_t Get32(const std::string& s, size_t at)
{
    uint32_t v;
    memcpy(&v, &s[at], 4);
    return v;
}

} // namespace

bool BackupAuthorKey(const std::string& password, std::string& file, std::string* error)
{
    if (password.size() < kMinBackupPassword)
        return Fail(error, "the password: at least " + std::to_string(kMinBackupPassword) + " characters");
    AuthorInfo author;
    AuthorKey key;
    if (!LoadAuthor(author, &key, error))
        return false;
    std::string plain =
        json{{"seed", crypto::Hex(key.seed, 32)}, {"name", author.name}, {"license", author.license}}.dump();
    SecureZeroMemory(key.seed, sizeof key.seed);
    std::string header(kBackupHeader, '\0');
    memcpy(&header[0], kBackupMagic, kBackupMagicSize);
    Put32(header, kBackupMagicSize, kBackupBlocks);
    Put32(header, kBackupMagicSize + 4, kBackupPasses);
    uint8_t* salt = reinterpret_cast<uint8_t*>(&header[kBackupMagicSize + 8]);
    uint8_t* nonce = salt + 16;
    uint8_t pk[32];
    if (!crypto::RandomBytes(salt, 16 + 24))
        return Fail(error, "no random bytes from the system");
    if (!PasswordKey(password, salt, kBackupBlocks, kBackupPasses, pk)) {
        crypto_wipe(&plain[0], plain.size());
        return Fail(error, "no memory for the password's key (32 MB)");
    }
    std::string mac(16, '\0'), cipher(plain.size(), '\0');
    crypto_aead_lock(reinterpret_cast<uint8_t*>(&cipher[0]), reinterpret_cast<uint8_t*>(&mac[0]), pk, nonce,
                     reinterpret_cast<const uint8_t*>(header.data()), header.size(),
                     reinterpret_cast<const uint8_t*>(plain.data()), plain.size());
    crypto_wipe(pk, sizeof pk);
    crypto_wipe(&plain[0], plain.size());
    file = header + mac + cipher;
    return true;
}

bool RestoreAuthorKey(const std::string& file, const std::string& password, bool replace, AuthorInfo* info,
                      std::string* error)
{
    if (file.size() < kBackupHeader + 16 + 2 || file.size() > 4096 || file.compare(0, kBackupMagicSize, kBackupMagic) != 0)
        return Fail(error, "not an author key backup");
    uint32_t blocks = Get32(file, kBackupMagicSize), passes = Get32(file, kBackupMagicSize + 4);
    if (blocks < 8 || blocks > 262144 || passes < 1 || passes > 16)  // (at most 256 MB, as no editor writes more)
        return Fail(error, "an author key backup with parameters this editor does not use");
    const uint8_t* salt = reinterpret_cast<const uint8_t*>(&file[kBackupMagicSize + 8]);
    uint8_t pk[32];
    if (!PasswordKey(password, salt, blocks, passes, pk))
        return Fail(error, "no memory for the password's key");
    std::string plain(file.size() - kBackupHeader - 16, '\0');
    int bad = crypto_aead_unlock(reinterpret_cast<uint8_t*>(&plain[0]),
                                 reinterpret_cast<const uint8_t*>(&file[kBackupHeader]), pk, salt + 16,
                                 reinterpret_cast<const uint8_t*>(file.data()), kBackupHeader,
                                 reinterpret_cast<const uint8_t*>(&file[kBackupHeader + 16]), plain.size());
    crypto_wipe(pk, sizeof pk);
    if (bad)
        return Fail(error, "wrong password, or the backup was changed");
    json j = json::parse(plain, nullptr, false);
    crypto_wipe(&plain[0], plain.size());
    AuthorKey key;
    if (!j.is_object() || !j.value("seed", json()).is_string() || !j.value("name", json()).is_string() ||
        !j.value("license", json()).is_string() || !crypto::FromHex(j["seed"].get<std::string>(), key.seed, 32) ||
        !ValidManifestText(j["name"].get<std::string>(), 40) || !ValidManifestText(j["license"].get<std::string>(), 64))
        return Fail(error, "the backup's content is damaged");
    crypto::Ed25519PublicKey(key.seed, key.pub);
    AuthorInfo now{j["name"].get<std::string>(), j["license"].get<std::string>(), crypto::Hex(key.pub, 32)};
    std::wstring dir = DataDir(true);
    if (dir.empty()) {
        SecureZeroMemory(key.seed, sizeof key.seed);
        return Fail(error, "no folder for the author key (LocalAppData)");
    }
    AuthorInfo old;
    AuthorKey oldKey;
    bool haveOld = GetFileAttributesW((dir + L"author.key").c_str()) != INVALID_FILE_ATTRIBUTES;
    bool same = haveOld && LoadAuthor(old, &oldKey, nullptr) && old.keyHex == now.keyHex;
    SecureZeroMemory(oldKey.seed, sizeof oldKey.seed);
    if (haveOld && !same) {
        if (!replace) {
            SecureZeroMemory(key.seed, sizeof key.seed);
            return Fail(error, "this PC has another author key" +
                                   (old.keyHex.empty() ? std::string() : " [" + old.Fingerprint() + "]") +
                                   ": restore with replace to put the backup's [" + now.Fingerprint() +
                                   "] in its place (the other one is kept beside)");
        }
        std::string hex = RandomHex(4);
        std::wstring aside = L".old-" + std::wstring(hex.begin(), hex.end());
        MoveFileW((dir + L"author.key").c_str(), (dir + L"author.key" + aside).c_str());
        MoveFileW((dir + L"author.json").c_str(), (dir + L"author.json" + aside).c_str());
    }
    if (!same) {
        std::string sealed;
        bool ok = Protect(std::string(reinterpret_cast<char*>(key.seed), 32), sealed) &&
                  WriteNewFileW(dir + L"author.key", sealed, false);
        if (!ok) {
            SecureZeroMemory(key.seed, sizeof key.seed);
            return Fail(error, "the author key cannot be written to " + Utf8(dir));
        }
    }
    SecureZeroMemory(key.seed, sizeof key.seed);
    if (!WriteFileAtomic(dir + L"author.json", json{{"name", now.name}, {"license", now.license}, {"key", now.keyHex}}.dump(2)))
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

bool ExportLevelFolder(const std::string& gameId, const std::string& folder, std::string& package, Manifest* manifest,
                       std::string* error, std::string* note)
{
    if (note)
        note->clear();
    const GameInfo* game = FindGame(gameId);
    if (!game)
        return Fail(error, "no game " + gameId + " is known to the package code");
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
    std::string why;
    if (!game->checkLevel || !game->checkLevel(level.data, &why))  // what an installer would make of it
        return Fail(error, "the level would be refused where it is installed:\n" + why);
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
    // the laptop's photos the level names (mission.files): photo1.png .. photo5.png of its folder
    std::vector<std::pair<std::string, size_t>> extra = {{"README.md", kMaxReadme}, {"preview.png", kMaxPreview}};
    if (j.contains("mission") && j["mission"].is_object() && j["mission"].contains("files") &&
        j["mission"]["files"].is_array())
        for (const json& pf : j["mission"]["files"])
            if (pf.is_object() && pf.value("image", json()).is_string() && PhotoFileName(pf["image"].get<std::string>()))
                extra.push_back({pf["image"].get<std::string>(), kMaxPreview});
    for (const auto& [name, max] : extra) {
        PackageFile f{name};
        if (!Exists(folder + "\\" + name)) {
            if (PhotoFileName(name))
                return Fail(error, "the laptop's photo " + name + " the level names is not in its folder");
            continue;
        }
        if (!ReadFileA(folder + "\\" + name, f.data, max))
            return Fail(error, name + " is larger than " + std::to_string(max >> 10) + " KB");
        if (f.name == "preview.png" || PhotoFileName(f.name)) {  // the picture only: its text / EXIF chunks stay here (issue #8)
            f.data = CleanPng(f.data);
            if (f.data.empty())
                return Fail(error, name + " is not a PNG picture of at most " + std::to_string(kMaxPreviewSide) +
                                       " x " + std::to_string(kMaxPreviewSide));
        }
        files.push_back(std::move(f));
    }
    std::string userName = WindowsUserName();
    for (const PackageFile& f : files) {
        if (f.name == "preview.png" || PhotoFileName(f.name))
            continue;
        if (HoldsPrivatePath(f.data))
            return Fail(error, f.name + " holds a path of this PC (C:\\Users\\...: your Windows user name would go "
                                        "with the package); take it out first");
        if (note && note->empty() && HoldsWord(f.data, userName))
            *note = f.name + " mentions your Windows user name (\"" + userName + "\"): it goes with the package";
    }
    AuthorInfo author;
    AuthorKey key;
    if (!LoadAuthor(author, &key, error))
        return false;
    Manifest m;
    m.game = game->id;
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
        else if (FindGame(m.game) &&
                 ReadFileA(gameDir + "\\" + FindGame(m.game)->onlineDir + "\\" + m.id + "\\package.json", installed,
                           kMaxManifestBytes) &&
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
    const GameInfo* game = FindGame(m.game);
    if (!ValidOnlineId(m.id) || !game)  // (OpenPackage checked them; the folder's name comes from the id)
        return Fail(error, "not an opened package");
    if (GetFileAttributesW(GamePath(gameDir + "\\" + game->exe).c_str()) == INVALID_FILE_ATTRIBUTES)
        return Fail(error, gameDir + " is not " + game->name + "'s folder (no " + game->exe + ")");
    std::string online = gameDir + "\\" + game->onlineDir, mods = online.substr(0, online.find_last_of('\\'));
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
        if (name != "level.json" && name != "README.md" && name != "preview.png" && !PhotoFileName(name) &&
            !ValidScriptName(name))
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
