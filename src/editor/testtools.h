#pragma once
// Testing aids (Testing section of the editor, Lua h2.speed / h2.god /
// h2.invisible / h2.freeze, keys while playing):
//
//   game speed   SysInterface+0x193C: the factor every frame's time is
//                multiplied by before it is added to the game clock (+0x28;
//                the frame time itself is clamped, so very high factors give
//                less than asked)
//   frozen       the engine's own pause (playtest.h), also while playing
//   god mode     the game's GodMode cheat value (0x6A960C, not 0: Hitman's
//                hit points are not lowered, ZHITMAN2 damage 0x519390)
//   invisible    the debug value "Invisible" of ZLIST_CheckVisible (0x6AA05C,
//                not 0: no one is told he sees anyone, 0x48D210)
//
// They hold while a map is loaded (the main menu always runs at 1x, normal).
#include <string>

namespace h2::editor {

void InstallTestTools();

float GameSpeed();
void SetGameSpeed(float factor);  // 0.1 .. 8
bool GodMode();
void SetGodMode(bool on);
bool HitmanInvisible();
void SetHitmanInvisible(bool on);
// The testing aids used on the map since it loaded ("god mode, game speed";
// "": none): a run with them is not rated for real.
std::string AidsUsed();
// The mission completed: the aids used (if any) are shown over the
// statistics until the map is left.
void NoteMissionEnd();

} // namespace h2::editor
