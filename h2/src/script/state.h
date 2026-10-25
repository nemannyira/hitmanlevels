#pragma once
#include <string>

// A level's story state, kept with the game's saves.
//
//   h2.state.stage = 2               -- any values: strings, numbers, booleans,
//   h2.state.found = { "key", ... }  -- tables (lists 1..n or string keys)
//   h2.on("save", function(e) ... end)  -- just before a save (e.slot): update h2.state
//   h2.on("load", function(e) ... end)  -- a saved game was loaded (e.slot):
//                                        -- h2.state is as it was saved (the
//                                        -- level's scripts have run already)
//
// The game saves a slot as Save\<slot>.1 .. .3 (ZEngineDataBase::SaveGame
// 0x41AAD0); h2.state goes next to them as Save\<slot>.h2lua (JSON), read
// back in LoadGame2 0x41B1C0 (slot at SysInterface +0x18A8 while loading)
// and deleted with the slot (DeleteSaveGame 0x41AE90). A new map starts with
// an empty h2.state. Number keys of tables that are not lists come back as
// strings; functions are not kept.
namespace h2::script {

void InstallState();
// Every frame (the "load" event after the level's scripts ran).
void UpdateState();
// The level's scripts have run (level.cpp).
void OnLevelScriptsRan();
// The editor's own state kept with a saved game (its mission goals done): a
// JSON text under the reserved key "__editor" of the slot's .h2lua. `save`
// returns it ("" nothing to keep); `load` gets it back after a saved game was
// loaded ("" when the slot had none).
void SetSaveExtra(std::string (*save)(), void (*load)(const std::string&));
// Called when a saved game is being loaded, just before the objects get their
// saved state (the map is loaded fresh then: as its files have it).
void SetBeforeGameLoad(void (*fn)());

} // namespace h2::script
