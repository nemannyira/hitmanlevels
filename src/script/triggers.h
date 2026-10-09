#pragma once
#include <string>

// Timers, zones and keys for level scripts.
//
//   h2.time()                        game seconds since the map was loaded: it
//                                    stands while the world stands (editor
//                                    open, Freeze, the game's pause) and runs
//                                    with the game speed
//   local t = h2.after(5, function(t) ... end)        once, in 5 game seconds
//   local t = h2.every(2, function(t) ... end [, n])  every 2 s (n times)
//   t:cancel(), t:active()
//
//   local z = h2.zone{ x = 100, y = 0, z = 200, radius = 300,   -- a sphere
//                      on_enter = function(z, who) ... end,
//                      on_leave = function(z, who) ... end }
//   h2.zone{ min = {x1, y1, z1}, max = {x2, y2, z2}, ... }        -- a box
//   h2.zone{ object = h2.find("Car"), radius = 400, ... }         -- moves with it
//   who = "hitman" (default) | "characters" (everyone, Hitman too) |
//         an object | a list of objects
//   z:inside(obj), z:contains(x, y, z), z:remove()
//   (checked 10 times a game second; who: the object that came / went)
//
//   h2.subtitle(text [, seconds])    a subtitle line (default 5 game seconds;
//                                    nil / "" takes it away)
//
//   h2.on("key", function(e) ... end)   a key pressed while playing (editor
//                                       closed, the game in front; the game
//                                       gets it too): e.key ("K", "F5",
//                                       "Numpad1", "Space"...), e.vk
//
//   npc:carry(template [, function(item, npc) ... end])   the character
//       carries an item: when it dies, a working item of the template
//       (h2.item) lands by its body (checked 10 times a game second); false:
//       it is dead already. npc:carry(nil) takes them back,
//       npc:carrying() lists the templates.
//
// Timers, zones and carried items of the level's scripts go when they run
// again; all go when the map is left.
namespace h2::script {

void InstallTriggers();
// Every frame: the game clock, due timers, zones, keys.
void UpdateTriggers();
// As the event handlers (events.h): timers and zones made while an owner is
// set are tagged with it.
void SetTriggerOwner(const std::string& owner);
void ClearTriggers(const std::string& owner);

} // namespace h2::script
