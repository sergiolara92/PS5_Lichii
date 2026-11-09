// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// MacroEngine::Execute's use of macro_native.h (derived macro.cpp, headless/CMakeLists.txt).
// EDEN_NX2_MME_NATIVE (dev setting mme_native=off|check): 0 runs every macro through the JIT;
// "check" runs each call a replacement takes through the interpreter as well, on copies, logs
// any difference, and then lets the JIT run it.
#include <array>
#include <cstdlib>
#include <cstring>
#include <span>

#include "common/logging.h"
#include "core/core.h"
#include "macro_census.h"
#include "macro_native.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/nx2_profile.h"

namespace Eden::MacroNative {

enum class Mode { Off, On, Check };

// Read at the first macro call: on the console, dev-settings set it after static initialization.
// A plain variable (see macro_census.h): this runs on every macro call.
inline int g_mode = -1;
inline Mode GetMode() {
    if (g_mode < 0) [[unlikely]] {
        const char* value = std::getenv("EDEN_NX2_MME_NATIVE");
        g_mode = static_cast<int>(value == nullptr                  ? Mode::On
                                  : std::strcmp(value, "check") == 0 ? Mode::Check
                                  : value[0] == '0'                  ? Mode::Off
                                                                     : Mode::On);
    }
    return static_cast<Mode>(g_mode);
}

inline void CheckCall(Id id, Tegra::Engines::Maxwell3D& maxwell3d, std::span<const u32> code, u32 start,
                      std::span<const u32> parameters, std::span<const u32> dram) {
    static std::array<u64, Count> checks{}, differences{};
    ++checks[id];
    const std::string difference = Compare(id, code, start, parameters, dram, [&](u32 address) {
        return address < Tegra::Engines::Maxwell3D::Regs::NUM_REGS ? maxwell3d.GetRegisterValue(address) : 0U;
    });
    if (!difference.empty() && ++differences[id] <= 16) {
        LOG_ERROR(HW_GPU, "MME native {} at {:#x} differs: {}; parameters {:x}", kNames[id], start, difference,
                  fmt::join(parameters, ","));
    }
    if ((checks[id] & 0xffff) == 1) {
        LOG_INFO(HW_GPU, "MME native {}: {} calls checked, {} differences", kNames[id], checks[id], differences[id]);
    }
}

// True when a replacement ran the call (the macro's code must not run it again).
inline bool Execute(Core::System& system, Tegra::Engines::Maxwell3D& maxwell3d, std::span<const u32> code,
                    u64 generation, u32 start, std::span<const u32> parameters, std::span<u32> dram) {
    const Mode mode = GetMode();
    if (mode == Mode::Off) return false;
    const Id id = Find(code, generation, start);
    if (id == None) return false;
    if (mode == Mode::Check) {
        CheckCall(id, maxwell3d, code, start, parameters, dram);
        return false;
    }
    // What MacroEngine::Execute's emit does for the methods the replacements write.
    return Run(id, parameters, dram, [&](u32 address, u32 value) {
        ::Eden::MacroCensus::Emit census_emit{address, value};
        VideoCommon::Nx2Profile::Scope method_scope{VideoCommon::Nx2Profile::Zone::Method};
        maxwell3d.MacroWrite(system, address, value);
    });
}

} // namespace Eden::MacroNative
