// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>

namespace Prospero::Development {

// This lease belongs to one complete frontend load/teardown scope. Ordinary
// launches also acquire it, with active=false, so inherited selectors cannot
// activate a profile while any ordinary guest or its JITs are alive.
class ResearchProfileLease final {
public:
    ResearchProfileLease(bool active, bool pauth, bool remote, bool strict_memory_aborts = false) {
#if !defined(PROSPERO_NATIVE_STARTUP_RESEARCH) || !PROSPERO_NATIVE_STARTUP_RESEARCH
        if (pauth || remote || strict_memory_aborts) {
            throw std::runtime_error("Native startup research profiles are disabled in this build");
        }
#endif
        (void)strict_memory_aborts;
        // getenv storage can be invalidated by the first setenv. Complete all
        // potentially throwing copies before mutating any environment variable.
        for (std::size_t i = 0; i < Names.size(); ++i) {
            if (const char* value = std::getenv(Names[i])) previous[i] = std::string(value);
        }
        try {
            Set(Names[0], "0");
            Set(Names[1], active && pauth ? "1" : "0");
            Set(Names[2], active && remote ? "1" : "0");
#if defined(PROSPERO_NATIVE_STARTUP_RESEARCH) && PROSPERO_NATIVE_STARTUP_RESEARCH
            if (active) Set(Names[0], "1");
#endif
            installed = true;
        } catch (...) {
            RestoreOrAbort();
            throw;
        }
    }

    ~ResearchProfileLease() noexcept {
        if (installed) RestoreOrAbort();
    }

    ResearchProfileLease(const ResearchProfileLease&) = delete;
    ResearchProfileLease& operator=(const ResearchProfileLease&) = delete;
    ResearchProfileLease(ResearchProfileLease&&) = delete;
    ResearchProfileLease& operator=(ResearchProfileLease&&) = delete;

private:
    inline static constexpr std::array<const char*, 3> Names{
        "PROSPERO_RESEARCH_NATIVE_ROUTE_ACTIVE",
        "PROSPERO_RESEARCH_PAUTH_DISABLED_PROFILE",
        "PROSPERO_RESEARCH_REMOTE_REGION_PROFILE",
    };
    std::array<std::optional<std::string>, 3> previous{};
    bool installed = false;

    static void Set(const char* name, const char* value) {
        if (::setenv(name, value, 1) != 0) {
            throw std::runtime_error(std::string("Cannot install native research selector: ") + name);
        }
    }

    [[noreturn]] static void RestoreFailed(const char* name) noexcept {
        const int error = errno;
        std::fprintf(stderr, "PROSPERO_RESEARCH profile_restore_failed selector=%s errno=%d action=abort\n",
                     name, error);
        std::fflush(stderr);
        std::abort();
    }

    void RestoreOne(std::size_t i) noexcept {
        const int result = previous[i] ? ::setenv(Names[i], previous[i]->c_str(), 1)
                                       : ::unsetenv(Names[i]);
        if (result != 0) RestoreFailed(Names[i]);
    }

    void RestoreOrAbort() noexcept {
        // Never expose partially restored profile values under an active route.
        if (::setenv(Names[0], "0", 1) != 0) RestoreFailed(Names[0]);
        RestoreOne(1);
        RestoreOne(2);
        RestoreOne(0);
        installed = false;
    }
};

} // namespace Prospero::Development