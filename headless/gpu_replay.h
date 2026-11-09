// SPDX-License-Identifier: GPL-3.0-or-later
// GPU command capture and replay (coordination/notes/gpu-replay.md).
#pragma once

namespace Core {
class System;
namespace Frontend {
class EmuWindow;
}
} // namespace Core

namespace Eden {
/// PC: starts recording when EDEN_HOST_GPU_CAPTURE names a file (gpu_replay.cpp). Call before the
/// game loads, so channels, address spaces and device mappings are followed from the start.
void StartGpuCapture(Core::System& system);
/// PC: replays a capture through this system's GPU with no program; returns the exit status.
int RunGpuReplay(Core::System& system, Core::Frontend::EmuWindow& window, const char* path);
} // namespace Eden
