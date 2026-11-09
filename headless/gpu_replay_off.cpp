// SPDX-License-Identifier: GPL-3.0-or-later
// GPU capture/replay (gpu_replay.cpp) for a source without its hooks (src/video_core/replay_hooks.h, patch 0007 on
// Eglantine; not yet ported to the Lichii base): capture does nothing and a replay reports it is unavailable.
#include <cstdio>
#include "gpu_replay.h"

namespace Eden {
void StartGpuCapture(Core::System&) {}
int RunGpuReplay(Core::System&, Core::Frontend::EmuWindow&, const char* path) {
    std::fprintf(stderr, "GPU replay unavailable in this build (no replay hooks): %s\n", path ? path : "");
    return 1;
}
} // namespace Eden
