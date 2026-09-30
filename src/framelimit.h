#pragma once

namespace h2 {

// Caps the engine main loop. Uncapped, the game runs at thousands of fps on
// modern hardware, which starves its time-based logic (scenes/cutscenes crawl,
// scene transitions can stall forever).
void InstallFrameLimiter(double fps);

} // namespace h2
