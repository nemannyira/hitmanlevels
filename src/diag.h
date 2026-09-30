#pragma once

namespace h2 {

// Timing / IO instrumentation: per-second stats, scene changes and a stall
// watchdog that samples the main thread's stack when no frame completes.
void InstallDiagnostics();

} // namespace h2
