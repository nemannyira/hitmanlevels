// h2level_pack: level packages (.h2lvl, src/levelfmt/package.h) outside the game: the installer of
// passed-on levels (the Level Hub's later, docs/online_levels.md), and export / inspect for tests.
//
//   h2level_pack install <file.h2lvl> [--game <dir>] [--yes] [--allow-older] [--allow-other-author]
//   h2level_pack inspect <file.h2lvl>
//   h2level_pack export <level folder> <file.h2lvl>      (stamps the level's next package version)
//   h2level_pack author [<name> [<license>]]            (shows / makes this PC's author key)
//   h2level_pack selftest
//   h2level_pack <file.h2lvl>                           (a double click: install, asking)
//
// Exit codes: 0 done, 1 refused (a check failed), 2 usage / unreadable, 3 needs a confirmation
// (--yes, --allow-older, --allow-other-author) that a non-interactive run did not give.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "../../src/levelfmt/crypto.h"
#include "../../src/levelfmt/package.h"
#include "../../src/levelfmt/share.h"

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

bool IsGameDir(const std::string& d) { return GetFileAttributesA((d + "\\hitman2.exe").c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string FindGameDir()
{
    char buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("H2_GAME_DIR", buf, MAX_PATH);
    if (n && n < MAX_PATH)
        return buf;
    if (IsGameDir(ExeDir()))
        return ExeDir();
    std::string steam = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Hitman 2 Silent Assassin";
    return IsGameDir(steam) ? steam : "";
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
    std::printf("Level:    %s (version %d)\n", m.title.c_str(), m.version);
    std::printf("Author:   %s  [%s]\n", m.authorName.c_str(), Fingerprint(m.authorKey).c_str());
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

int Install(const std::string& path, std::string game, bool yes, bool allowOlder, bool allowOther, bool interactive)
{
    OpenedPackage p;
    if (!Open(path, p))
        return 1;
    Show(p);
    if (game.empty())
        game = FindGameDir();
    if (game.empty() || !IsGameDir(game)) {
        std::fprintf(stderr, "The game's folder is not found: give it with --game <dir>\n");
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
        std::printf("\nAn author this PC has not seen before: %s [%s].\n"
                    "The level runs with limits like every passed-on level (no console, less memory and time).\n",
                    m.authorName.c_str(), Fingerprint(m.authorKey).c_str());
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

int Export(const std::string& folder, const std::string& out)
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
    std::string package;
    Manifest m;
    if (!ExportLevelFolder(folder, package, &m, &error)) {
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
    return 0;
}

int Author(int argc, char** argv)
{
    AuthorInfo a;
    std::string error;
    if (argc >= 3) {
        std::string license = argc >= 4 ? argv[3] : LicenseChoices()[0];
        if (!SetAuthor(argv[2], license, &a, &error)) {
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

int Usage()
{
    std::fprintf(stderr,
                 "usage: h2level_pack install <file.h2lvl> [--game <dir>] [--yes] [--allow-older] [--allow-other-author]\n"
                 "       h2level_pack inspect <file.h2lvl>\n"
                 "       h2level_pack export <level folder> <file.h2lvl>\n"
                 "       h2level_pack author [<name> [<license>]]\n"
                 "       h2level_pack selftest\n");
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);  // (authors' names, titles)
    if (argc < 2)
        return Usage();
    std::string cmd = argv[1];
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
        TrustCheck t = CheckTrust(p, FindGameDir());
        static const char* kinds[] = {"an author not seen on this PC", "a known author", "ANOTHER author than the "
                                      "installed level's", "an older version than the installed one",
                                      "installed already"};
        std::printf("This PC:  %s\n", kinds[t.kind]);
        return 0;
    }
    if (cmd == "export" && argc == 4)
        return Export(argv[2], argv[3]);
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
    if (argc == 2 && cmd.size() > 6 && _stricmp(cmd.c_str() + cmd.size() - 6, ".h2lvl") == 0) {
        g_pause = true;  // a double click
        return Done(Install(cmd, "", false, false, false, true));
    }
    return Usage();
}
