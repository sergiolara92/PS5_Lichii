// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "common/settings_setting.h"
namespace Prospero::Development {
// The actual pinned cpuopt_ignore_memory_aborts is a global Setting<bool>,
// not SwitchableSetting. Never reinterpret a custom/global selection layer.
class StrictMemoryAbortLease final {
public:
    explicit StrictMemoryAbortLease(Settings::Setting<bool>& setting) : setting(setting) {}
    ~StrictMemoryAbortLease() noexcept { RestoreOrAbort(); }
    StrictMemoryAbortLease(const StrictMemoryAbortLease&) = delete;
    StrictMemoryAbortLease& operator=(const StrictMemoryAbortLease&) = delete;
    StrictMemoryAbortLease(StrictMemoryAbortLease&&) = delete;
    StrictMemoryAbortLease& operator=(StrictMemoryAbortLease&&) = delete;
    void Activate() {
#if !defined(PROSPERO_NATIVE_STARTUP_RESEARCH) || !PROSPERO_NATIVE_STARTUP_RESEARCH
        throw std::runtime_error("Native strict memory research profile is disabled in this build");
#else
        if (active) return;
        if (setting.Switchable())
            throw std::runtime_error("Strict memory lease requires the reviewed global Setting<bool> type");
        previous = setting.GetValue();
        active = true;
        try { setting.SetValue(false); }
        catch (...) { RestoreOrAbort(); throw; }
#endif
    }
    bool Active() const noexcept { return active; }
private:
    Settings::Setting<bool>& setting;
    bool previous = false;
    bool active = false;
    void RestoreOrAbort() noexcept {
        if (!active) return;
        try { setting.SetValue(previous); active = false; }
        catch (...) {
            std::fputs("NATIVE_RESEARCH strict_memory_setting_restore_failed action=abort\n", stderr);
            std::abort();
        }
    }
};
} // namespace Prospero::Development
