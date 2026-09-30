#pragma once

namespace h2 {

using FrameCallback = void (*)();

// Register a callback run after every ZEngineDataBase::MainLoop iteration
// (in registration order). Hooks MainLoop on first use.
void OnFrameEnd(FrameCallback cb);

} // namespace h2
