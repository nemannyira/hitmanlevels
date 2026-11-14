#pragma once
// The plays of downloaded levels sent to the site (docs/play_stats.md section 2, #51): the game's editor
// writes each play to <game>\Mods\Online\<id>\runs\<started>.json; levelpack posts the ones not sent yet,
// when the player turned sharing on (share.h StatsSharing). A sent run goes to runs\sent, one the site refused
// to runs\refused; one with no answer waits for the next time.
//
// Never sent (kept on the PC only): a run played with testing aids or dev settings, one of another version than
// the installed one, one of a level whose files changed since levelpack installed it (package.json's sizes and
// SHA-256) or that levelpack did not install (no package hash in known.json). The site gets the run as written
// plus the game and the installed package's SHA-256 (the site's list names it).
#include <string>
#include <vector>

#include "games.h"

namespace h2::levelfmt {

constexpr const char* kRunsPath = "/api/v1/runs";

struct RunsReport {
    int sent = 0;       // stored by the site now (or there already)
    int refused = 0;    // refused by it now
    int waiting = 0;    // to send (not sent yet: no answer, or only counted)
    int kept = 0;       // never sent (see above)
    int sentBefore = 0; // in runs\sent already
    std::vector<std::string> notes;  // a line for each run kept, refused or left waiting
};

// The runs of `game`'s downloaded levels in `gameDir`: `send` posts the ones to send to `source` ("https://host"
// or a folder: hub.h Post); otherwise they are only counted. `token`: levelpack signed in (the runs carry the
// account; a run's ticket, runs\<run>.ticket, goes with it and with its file to runs\sent).
RunsReport SendRuns(const std::string& source, const std::string& gameDir, const GameInfo& game, bool send,
                    const std::string& token = "");

// A ticket for a run starting now (signed in: the site notes its time, docs/play_stats.md section 4), written to
// <game>\Mods\Online\<level>\runs\<run file's name>.ticket for the run's sending.
bool TakeTicket(const std::string& source, const std::string& token, const std::string& gameDir, const GameInfo& game,
                const std::string& level, int version, const std::string& runFile, std::string* error);

} // namespace h2::levelfmt
