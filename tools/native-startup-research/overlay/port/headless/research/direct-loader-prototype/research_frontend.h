// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "development_input.h"
#include "research_descriptor.h"
#include "research_profile_lease.h"
#include "research_cpu_setting_lease.h"

namespace Prospero::Development {
class FactoryLease {
public:
    explicit FactoryLease(u64 token = 0) : token(token) {}
    ~FactoryLease();
    FactoryLease(const FactoryLease&) = delete;
    FactoryLease& operator=(const FactoryLease&) = delete;
    FactoryLease(FactoryLease&& other) noexcept;
    FactoryLease& operator=(FactoryLease&& other) noexcept;
private:
    u64 token{};
};
struct ResearchLoadResult {
    bool mapped = false;
    Loader::ResultStatus status = Loader::ResultStatus::ErrorNotInitialized;
    std::size_t modules = 0;
    std::string detail;
};
class ResearchRoute {
public:
    explicit ResearchRoute(const std::filesystem::path& descriptor_file);
    bool Pending() const;
    bool Invalid() const;
    const std::string& Error() const;
    ResearchMode Mode() const;
    bool PAuthDisabledProfile() const;
    bool RemoteRegionProfile() const;
    bool VerifiedExtendedMemoryProfile() const;
    bool StrictMemoryAbortsRequested() const;
    // Consumes the one research request; failure never substitutes another ROM.
    std::string TakeEntryPoint();
    FactoryLease InstallFactory(Core::System& expected_system, StrictMemoryAbortLease* strict_memory = nullptr) const;
    ResearchLoadResult PerformLoadOnly() const;
    void CheckExecuteEnabled() const;
private:
    struct State;
    std::shared_ptr<State> state;
};
} // namespace Prospero::Development
