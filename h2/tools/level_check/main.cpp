// h2level_check: what the editor DLL would make of a level file, from outside the game (the
// Level Hub's installer starts from this; docs/online_levels.md).
//
//   h2level_check [--strict] <level.json | level folder>...
//
// Exit code 0: every level is accepted, 1: one is refused, 2: usage / unreadable.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "../../src/levelfmt/level_check.h"

int main(int argc, char** argv)
{
    bool strict = false;
    int checked = 0, refused = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--strict") {
            strict = true;
            continue;
        }
        std::string path = arg;
        if (path.size() < 5 || path.compare(path.size() - 5, 5, ".json") != 0)
            path += "\\level.json";
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "%s: cannot read\n", path.c_str());
            return 2;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        h2::levelfmt::Report r = h2::levelfmt::CheckLevelText(ss.str(), strict);
        ++checked;
        refused += r.Ok() ? 0 : 1;
        std::printf("%s: %s (%zu issues)\n%s", path.c_str(), r.Ok() ? "accepted" : "REFUSED", r.issues.size(),
                    r.Summary(20).c_str());
    }
    if (!checked) {
        std::fprintf(stderr, "usage: h2level_check [--strict] <level.json | level folder>...\n");
        return 2;
    }
    return refused ? 1 : 0;
}
