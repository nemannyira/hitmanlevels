# Courier (example mission, C1-1)

A courier walks his round at the front gate with papers. Stop him, take the
papers he drops and get back to the hill where you started. If he is not
stopped in three minutes he leaves and the mission fails. Saved games keep
the story (`h2.state`), the time left too.

Play: copy this folder into the game's `Mods` folder and start "Courier" from
the editor's launcher.

Made with: a character placed in the level (`level.json`, no behaviour: the
script walks him), `npc:walkto` / `moveset` / `lookat`, `npc:carry` (the
papers), the `death` and `pickup` events, a zone (the way out), timers, the
laptop's lines (`h2.objective`, the level's own fourth hidden), subtitles,
`h2.state` with the `save` event; the base level's own mission logic cannot
end it (`base_ends`).

## What writing it showed (missing or awkward)

* **The path network could not be seen**: the courier's round uses points
  found by trial (a point off the network: the character stays). Now the
  editor shows it (Testing panel > Path network) and `h2.onpath(x, y, z)`
  tells whether a point is on it.
* **A character could not be given an item**: now `npc:carry(template)`
  (the item falls by the body when he dies, as the game's guards drop their
  weapons). Items made while playing are not in the game's saves: the
  script puts the papers back after a load (`h2.state.papers`).
* **The base level's own story still ran**: C1-1's level control could end
  the mission on its own (the Don killed, ...). Now off for this mission:
  `"mission": { "base_ends": false }` (h2.basemission).
* **No on-screen info / hint box**: subtitles work for short lines; the
  game's "INFO:" box takes text keys (LOC), not text.
* **The map's own name and progress**: completing it counts as completing
  C1-1 in the profile (a map of its own: docs/roadmap.md section 3).
