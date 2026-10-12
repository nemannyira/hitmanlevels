# The Informant (C1-1)

A delivery boy who saw the Don's men bury a body waits on the east road.
Find him and take him to the postman's corner by the south road, where a car
waits. He follows you: keep him close (more than 40 m away for 20 seconds and
he runs off) and keep him alive.

Play: copy this folder into the game's `Mods` folder, then main menu > Level
Editor > Open a level > *Play*.

Made with: the informant (NPCs tab: DeliveryBoy, no behaviour), a point at
the meeting place. `main.lua`: a zone around him (`h2.zone{ object = ... }`),
following with `npc:walkto(h2.walkable(...))` and `moveset`, the `death`
event, laptop lines 5 and 6 (C1-1's own hidden).

Played through 2026-10-12 (docs/playtest_log.md): completed; failed when he
died; failed when he was left behind.
