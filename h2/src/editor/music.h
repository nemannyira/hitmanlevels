#pragma once
// Level music.
//
// A level's music are box zones under its MusicControl group, one per mood
// and zone (Location_01, PreAction_01, Action_01: calm, tension, combat),
// each with a sound reference into the level's sound table (.SND, kept in
// memory by the game; a reference is the offset of an entry's fields after
// its name: {1, name offset, ...}). Every level lists all the music streams
// (Streams/Music/*.wav). Each zone's MusicSegment routine keeps its sound
// reference (+0x7A, as the game's load of its data puts it); the editor gives
// it another track there, and the MusicControl takes Hitman's zone as entered
// anew, sending its segments to the level's LevelControl, which plays them.
#include <cstdint>
#include <string>
#include <vector>

namespace h2::editor {

struct MusicTrack {
    std::string name;  // "RussiaSlow" (file Streams/Music/RussiaSlow.wav)
    uint32_t ref = 0;  // in this level's sound table
};

struct MusicSlot {
    int record = -1;
    std::string path;      // the music object
    std::string zone;      // its group under MusicControl
    std::string mood;      // "Location", "PreAction", "Action"
    uint32_t original = 0; // the level's track (reference)
    uint32_t current = 0;
};

const std::vector<MusicTrack>& MusicTracks();
std::vector<MusicSlot> MusicSlots();
std::string TrackName(uint32_t ref);
// Gives a music object another track (runtime; the level document keeps it by track name).
bool SetMusicTrack(const std::string& slotPath, const std::string& track, std::string* error = nullptr);

// "Music" section (drawn in the Mission panel) and applying the level's music.
void DrawMusicSection();
void ApplyMusic();
void RevertMusic();
void InstallMusic();

} // namespace h2::editor
