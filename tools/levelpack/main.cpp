// levelpack: level packages (shared/levelfmt/package.h) of every game outside the games: the installer of
// passed-on levels (the Level Hub's, docs/online_levels.md), export / inspect, the site's signing.
// A package's game (its manifest's "game": h2 = Hitman 2, .h2lvl) tells which game's folder it goes to.
//
//   levelpack install <package> [--game <dir>] [--yes] [--allow-older] [--allow-other-author]
//   levelpack inspect <package>
//   levelpack export <level folder> <package> [--for h2]     (stamps the level's next package version)
//   levelpack author [<name> [<license>]]                    (shows / makes this PC's author key)
//   levelpack author backup <file.h2key>                     (the key with a password, asked or piped)
//   levelpack author restore <file.h2key> [--replace]
//   levelpack hub list|install <id> [--source https://hitmanlevels.com | <folder>]   (the site's levels)
//   levelpack site keygen <key file> | site sign <index.json> <key file>             (the site's operator)
//   levelpack selftest
//   levelpack --version
//   levelpack <package>                                      (a double click: install, asking)
//
// Exit codes: 0 done, 1 refused (a check failed), 2 usage / unreadable, 3 needs a confirmation
// (--yes, --allow-older, --allow-other-author) that a non-interactive run did not give.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <shellapi.h>

#include <cstdio>
#include <io.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "levelfmt/crypto.h"
#include "levelfmt/games.h"
#include "levelfmt/h2_game.h"
#include "hitmanlevels_version.h"
#include "levelfmt/hub.h"
#include "levelfmt/package.h"
#include "levelfmt/share.h"

using namespace h2::levelfmt;

namespace {

bool g_pause = false;  // started by a double click: the window stays until Enter

int Done(int code)
{
    if (g_pause) {
        std::printf("\nPress Enter to close.");
        std::string line;
        std::getline(std::cin, line);
    }
    return code;
}

bool ReadAll(const std::string& path, std::string& out, size_t max)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f || f.tellg() > std::streamoff(max))
        return false;
    out.resize(size_t(f.tellg()));
    f.seekg(0);
    return bool(f.read(out.data(), std::streamsize(out.size())));
}

std::string ExeDir()
{
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf, n);
    return s.substr(0, s.find_last_of('\\'));
}

bool IsGameDir(const std::string& d, const GameInfo& g)
{
    return GetFileAttributesA((d + "\\" + g.exe).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// a game's folder: <ID>_GAME_DIR (H2_GAME_DIR), this program's own folder, or Steam's usual place
std::string FindGameDir(const GameInfo& g)
{
    char buf[MAX_PATH];
    std::string var = g.id + "_GAME_DIR";
    for (char& c : var)
        c = char(toupper(static_cast<unsigned char>(c)));
    DWORD n = GetEnvironmentVariableA(var.c_str(), buf, MAX_PATH);
    if (n && n < MAX_PATH)
        return buf;
    if (IsGameDir(ExeDir(), g))
        return ExeDir();
    std::string steam = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\" + g.steamFolder;
    return IsGameDir(steam, g) ? steam : "";
}

bool Ask(const char* question, bool interactive)
{
    if (!interactive)
        return false;
    std::printf("%s [y/N] ", question);
    std::string line;
    std::getline(std::cin, line);
    return line == "y" || line == "Y" || line == "yes" || line == "igen" || line == "i";
}

void Show(const OpenedPackage& p)
{
    const Manifest& m = p.manifest;
    std::printf("Game:     %s\n", FindGame(m.game) ? FindGame(m.game)->name.c_str() : m.game.c_str());
    std::printf("Level:    %s (version %d)\n", m.title.c_str(), m.version);
    std::printf("Author:   [%s]  %s\n", Fingerprint(m.authorKey).c_str(), m.authorName.c_str());
    std::printf("License:  %s\n", m.license.c_str());
    std::printf("Map:      %s\n", m.baseScene.c_str());
    std::printf("Id:       %s\n", m.id.c_str());
    std::printf("Files:   ");
    for (const PackageFile& f : p.files)
        std::printf(" %s (%zu)", f.name.c_str(), f.data.size());
    std::printf("\n");
}

bool Open(const std::string& path, OpenedPackage& p)
{
    std::string bytes, error;
    if (!ReadAll(path, bytes, kMaxPackageBytes)) {
        if (std::ifstream(path, std::ios::binary))
            std::fprintf(stderr, "REFUSED: larger than %zu MB: not a level package\n", kMaxPackageBytes >> 20);
        else
            std::fprintf(stderr, "%s: cannot read it\n", path.c_str());
        return false;
    }
    if (!OpenPackage(bytes, p, &error)) {
        std::fprintf(stderr, "REFUSED: %s\n", error.c_str());
        return false;
    }
    return true;
}

int InstallOpened(const OpenedPackage& p, std::string game, bool yes, bool allowOlder, bool allowOther,
                  bool interactive);

int Install(const std::string& path, std::string game, bool yes, bool allowOlder, bool allowOther, bool interactive)
{
    OpenedPackage p;
    if (!Open(path, p))
        return 1;
    return InstallOpened(p, game, yes, allowOlder, allowOther, interactive);
}

int InstallOpened(const OpenedPackage& p, std::string game, bool yes, bool allowOlder, bool allowOther,
                  bool interactive)
{
    Show(p);
    const GameInfo& info = *FindGame(p.manifest.game);  // (OpenPackage refuses a game not registered)
    if (game.empty())
        game = FindGameDir(info);
    if (game.empty() || !IsGameDir(game, info)) {
        std::fprintf(stderr, "%s's folder is not found: give it with --game <dir>\n", info.name.c_str());
        return 2;
    }
    TrustCheck t = CheckTrust(p, game);
    const Manifest& m = p.manifest;
    switch (t.kind) {
    case TrustCheck::SameVersion:
        std::printf("\nThis version is installed already.\n");
        return 0;
    case TrustCheck::OtherAuthor:
        std::printf("\nWARNING: a level with this id is installed from ANOTHER author (key %s).\n"
                    "This package may be someone else's level passed off as an update of it.\n",
                    Fingerprint(t.installedKey).c_str());
        if (!allowOther && !Ask("Replace it anyway?", interactive)) {
            std::printf("Not installed.\n");
            return interactive ? 1 : 3;
        }
        break;
    case TrustCheck::Older:
        std::printf("\nThis is an OLDER version (%d) than one installed before (%d).\n", m.version, t.highestVersion);
        if (!allowOlder && !Ask("Go back to it?", interactive)) {
            std::printf("Not installed.\n");
            return interactive ? 1 : 3;
        }
        break;
    case TrustCheck::NewAuthor:
        std::printf("\nAn author this PC has not seen before: [%s] %s.\n"
                    "The level runs with limits like every passed-on level (no console, less memory and time).\n",
                    Fingerprint(m.authorKey).c_str(), m.authorName.c_str());
        if (!yes && !Ask("Install it?", interactive)) {
            std::printf("Not installed.\n");
            return interactive ? 1 : 3;
        }
        break;
    case TrustCheck::KnownAuthor:
        if (!t.knownName.empty() && t.knownName != m.authorName)
            std::printf("\n(The author's key was first seen named \"%s\".)\n", t.knownName.c_str());
        break;
    }
    std::string error;
    if (!InstallPackage(p, game, &error)) {
        std::fprintf(stderr, "Not installed: %s\n", error.c_str());
        return 1;
    }
    std::printf("\nInstalled: Mods\\Online\\%s (in the editor: File > Open level).\n", m.id.c_str());
    return 0;
}

int Export(const std::string& folder, const std::string& out, const std::string& gameId)
{
    std::string text, error;
    if (!ReadAll(folder + "\\level.json", text, kMaxPackagedLevel)) {
        std::fprintf(stderr, "%s\\level.json: cannot read it\n", folder.c_str());
        return 2;
    }
    nlohmann::json level;
    try {
        level = nlohmann::json::parse(text);
    } catch (const std::exception&) {
        std::fprintf(stderr, "level.json is not JSON\n");
        return 1;
    }
    if (!StampPackage(level, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    {
        std::ofstream f(folder + "\\level.json", std::ios::binary);
        f << level.dump(2);
    }
    std::string package, note;
    Manifest m;
    if (!ExportLevelFolder(gameId, folder, package, &m, &error, &note)) {
        std::ofstream f(folder + "\\level.json", std::ios::binary);  // (the version not used: as it was)
        f << text;
        std::fprintf(stderr, "Not exported: %s\n", error.c_str());
        return 1;
    }
    std::ofstream f(out, std::ios::binary);
    if (!f.write(package.data(), std::streamsize(package.size()))) {
        std::fprintf(stderr, "%s: cannot write it\n", out.c_str());
        return 2;
    }
    std::printf("%s: %s version %d, %zu bytes, signed by %s [%s]\n", out.c_str(), m.title.c_str(), m.version,
                package.size(), m.authorName.c_str(), Fingerprint(m.authorKey).c_str());
    if (!note.empty())
        std::printf("Note: %s\n", note.c_str());
    return 0;
}

// a password: from a pipe as it is (tests), else asked without echo (twice when `confirm`)
// a password: one line of the standard input (piped, or typed: the editor's Share panel asks it hidden)
std::string ReadPassword(const char* prompt, bool confirm)
{
    std::string line, again;
    std::printf("%s", prompt);
    std::getline(std::cin, line);
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (confirm && _isatty(_fileno(stdin))) {
        std::printf("Again: ");
        std::getline(std::cin, again);
        if (!again.empty() && again.back() == '\r')
            again.pop_back();
        if (again != line)
            return "";
    }
    return line;
}

int AuthorBackup(int argc, char** argv)
{
    std::string error, file;
    if (argv[2] == std::string("backup")) {
        std::string password = ReadPassword("Password for the backup (at least 8 characters): ", true);
        if (!BackupAuthorKey(password, file, &error)) {
            std::fprintf(stderr, "Not backed up: %s\n", error.empty() ? "the passwords differ" : error.c_str());
            return 1;
        }
        std::ofstream f(argv[3], std::ios::binary);
        if (!f.write(file.data(), std::streamsize(file.size()))) {
            std::fprintf(stderr, "%s: cannot write it\n", argv[3]);
            return 2;
        }
        std::printf("Backed up to %s: keep it and its password; with them another PC signs as you.\n", argv[3]);
        return 0;
    }
    bool replace = argc >= 5 && argv[4] == std::string("--replace");
    if (!ReadAll(argv[3], file, 4096)) {
        std::fprintf(stderr, "%s: cannot read it\n", argv[3]);
        return 2;
    }
    AuthorInfo a;
    if (!RestoreAuthorKey(file, ReadPassword("Password of the backup: ", false), replace, &a, &error)) {
        std::fprintf(stderr, "Not restored: %s\n", error.c_str());
        return 1;
    }
    std::printf("Restored: %s  [%s]\n", a.name.c_str(), a.Fingerprint().c_str());
    return 0;
}

// argument i as UTF-8 (argv is in the code page: names and licenses keep every letter so)
std::string Utf8Arg(int i)
{
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    std::string out;
    if (w && i < n) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        out.resize(len > 0 ? size_t(len - 1) : 0);
        WideCharToMultiByte(CP_UTF8, 0, w[i], -1, out.data(), len, nullptr, nullptr);
    }
    LocalFree(w);
    return out;
}

int Author(int argc, char** argv)
{
    AuthorInfo a;
    std::string error;
    if (argc >= 4 && (argv[2] == std::string("backup") || argv[2] == std::string("restore")))
        return AuthorBackup(argc, argv);
    if (argc >= 3) {
        std::string license = argc >= 4 ? Utf8Arg(3) : LicenseChoices()[0];
        if (!SetAuthor(Utf8Arg(2), license, &a, &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
    } else if (!LoadAuthor(a, nullptr, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("Author:  %s  [%s]\nLicense: %s\nKey:     %s\nFolder:  %s\n", a.name.c_str(), a.Fingerprint().c_str(),
                a.license.c_str(), a.keyHex.c_str(), ShareDataDir().c_str());
    return 0;
}

int Usage();

// levelpack hub list|install ...: the site's levels (src/levelfmt/hub.h)
int Hub(int argc, char** argv)
{
    std::string source = kHubSite, game, id;
    bool yes = false, older = false, other = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--source" && i + 1 < argc)
            source = argv[++i];
        else if (a == "--game" && i + 1 < argc)
            game = argv[++i];
        else if (a == "--yes")
            yes = true;
        else if (a == "--allow-older")
            older = true;
        else if (a == "--allow-other-author")
            other = true;
        else if (id.empty() && a[0] != '-')
            id = a;
        else
            return Usage();
    }
    HubIndex index;
    std::string error;
    if (!LoadHubIndex(source, index, &error)) {
        std::fprintf(stderr, "REFUSED: %s\n", error.c_str());
        return 1;
    }
    if (argv[2] == std::string("list")) {
        std::printf("%zu levels (list %lld):\n", index.levels.size(), index.serial);
        for (const HubLevel& l : index.levels)
            std::printf("  %-32s v%-3d [%s] %s  %s\n", l.id.c_str(), l.version, Fingerprint(l.authorKey).c_str(),
                        l.authorName.c_str(), l.title.c_str());
        return 0;
    }
    const HubLevel* level = index.Find(id);
    if (!level) {
        std::fprintf(stderr, "no level %s on the site's list\n", id.c_str());
        return 1;
    }
    OpenedPackage p;
    if (!FetchHubPackage(source, *level, p, &error)) {
        std::fprintf(stderr, "REFUSED: %s\n", error.c_str());
        return 1;
    }
    std::printf("From %s (the site's signed list %lld).\n", source.c_str(), index.serial);
    return InstallOpened(p, game, yes, older, other, false);
}

// levelpack site keygen|sign ...: the site operator's offline signing (src/levelfmt/hub.h)
int Site(int argc, char** argv)
{
    std::string error;
    if (argv[2] == std::string("keygen") && argc == 4) {
        std::string pub;
        if (!MakeSiteKey(argv[3], &pub, &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::printf("Site key made: %s (sealed for this Windows user)\nPublic key: %s\n", argv[3], pub.c_str());
        return 0;
    }
    if (argv[2] == std::string("sign") && argc == 5) {
        std::string text, sig;
        if (!ReadAll(argv[3], text, kMaxIndexBytes) || !SignIndex(text, argv[4], sig, &error)) {
            std::fprintf(stderr, "Not signed: %s\n", error.empty() ? "cannot read the index" : error.c_str());
            return 1;
        }
        std::string out = std::string(argv[3]);
        out = out.substr(0, out.size() - (out.size() > 5 && out.compare(out.size() - 5, 5, ".json") == 0 ? 5 : 0)) + ".sig";
        std::ofstream f(out, std::ios::binary);
        if (!f.write(sig.data(), std::streamsize(sig.size()))) {
            std::fprintf(stderr, "%s: cannot write it\n", out.c_str());
            return 2;
        }
        std::printf("Signed: %s\n", out.c_str());
        return 0;
    }
    return Usage();
}

int Usage()
{
    std::fprintf(stderr,
                 "usage: levelpack install <package> [--game <dir>] [--yes] [--allow-older] [--allow-other-author]\n"
                 "       levelpack inspect <package>\n"
                 "       levelpack export <level folder> <package> [--for h2]\n"
                 "       levelpack author [<name> [<license>]]\n"
                 "       levelpack author backup <file.h2key>\n"
                 "       levelpack author restore <file.h2key> [--replace]\n"
                 "       levelpack hub list [--source https://host | <folder>]\n"
                 "       levelpack hub install <id> [--source ...] [--game <dir>] [--yes] [--allow-older] "
                 "[--allow-other-author]\n"
                 "       levelpack site keygen <key file>\n"
                 "       levelpack site sign <index.json> <key file>\n"
                 "       levelpack selftest\n"
                 "       levelpack --version\n");
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);  // (authors' names, titles)
    RegisterH2Game();  // the games whose packages it knows (shared/levelfmt/games.h)
    if (argc < 2)
        return Usage();
    std::string cmd = argv[1];
    if (cmd == "--version") {
        std::printf("levelpack (Hitman Levels) %s\n", HL_VERSION);
        return 0;
    }
    if (cmd == "selftest") {
        bool ok = h2::levelfmt::crypto::Ed25519SelfTest();
        std::printf("Ed25519 RFC 8032 vectors: %s\n", ok ? "ok" : "FAILED");
        return ok ? 0 : 1;
    }
    if (cmd == "author")
        return Author(argc, argv);
    if (cmd == "inspect" && argc == 3) {
        OpenedPackage p;
        if (!Open(argv[2], p))
            return 1;
        Show(p);
        TrustCheck t = CheckTrust(p, FindGameDir(*FindGame(p.manifest.game)));
        static const char* kinds[] = {"an author not seen on this PC", "a known author", "ANOTHER author than the "
                                      "installed level's", "an older version than the installed one",
                                      "installed already"};
        std::printf("This PC:  %s\n", kinds[t.kind]);
        return 0;
    }
    if (cmd == "export" && (argc == 4 || (argc == 6 && argv[4] == std::string("--for"))))
        return Export(argv[2], argv[3], argc == 6 ? argv[5] : "h2");
    if (cmd == "hub" && argc >= 3 && (argv[2] == std::string("list") || argv[2] == std::string("install")))
        return Hub(argc, argv);
    if (cmd == "site" && argc >= 4)
        return Site(argc, argv);
    if (cmd == "install" && argc >= 3) {
        std::string game;
        bool yes = false, older = false, other = false;
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--game" && i + 1 < argc)
                game = argv[++i];
            else if (a == "--yes")
                yes = true;
            else if (a == "--allow-older")
                older = true;
            else if (a == "--allow-other-author")
                other = true;
            else
                return Usage();
        }
        return Install(argv[2], game, yes, older, other, false);
    }
    if (argc == 2 && GameOfExtension(cmd)) {
        g_pause = true;  // a double click
        return Done(Install(cmd, "", false, false, false, true));
    }
    return Usage();
}
