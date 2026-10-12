# The Accountant (C1-1)

The Don's accountant paces the road by the garage. Eliminate him, then get
back to the hill where you start. C1-1's own guards are all there: shooting
in the open raises the alarm.

Play: copy this folder into the game's `Mods` folder, then main menu > Level
Editor > Open a level > *Play*.

Made with the editor only, but for the story script: the accountant (NPCs
tab, no behaviour), a point on the hill (the exit), *Make it a target* and
*Make it the exit* (Inspector > Mission), the objective's text (Mission tab).
`main.lua` walks him, hides C1-1's own laptop lines and turns off the base
level's own ending (`h2.basemission(false)`).

Played through 2026-10-12 (docs/playtest_log.md): completed, rated
Professional.
