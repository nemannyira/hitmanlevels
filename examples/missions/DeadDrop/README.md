# Dead Drop (C1-1, emptied map)

A package lies in the garage yard, watched by two guards. Take it and leave
it at the postman's corner by the road south within four minutes. Seen
twice, the drop is off.

The guards (BaseGuard) shoot a trespasser on sight: come from the south,
behind them, sneaking (Space).

Play: copy this folder into the game's `Mods` folder, then main menu > Level
Editor > Open a level > *Play*.

Made with: *Empty map* (Level tab), two guards (NPCs tab), the package
(Models tab > items > KeyRoom, laid on flat ground). `main.lua`: the laptop
lines, the time limit (kept with saved games), the `pickup`, `spotted` and
`hostile` events, the drop zone.

Played through 2026-10-12 (docs/playtest_log.md): completed; failed by the
time limit; failed when seen (the guards killed Hitman).
