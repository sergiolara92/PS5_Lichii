// OFF-by-default, exact-input-selected diagnostic HLE policy.
// This is not a statement of retail Switch2 remote-memory capacity or layout.
#pragma once
#include <cstdint>
#include <string_view>

namespace Kernel::ResearchRemotePolicy {
constexpr std::uint64_t ProgramId = 0x0400c3f00006e000ULL; // Verified input main.npdm ACI0.
constexpr std::uint64_t ReservedBytes = 64ULL * 1024 * 1024 * 1024;
enum class Selection { Off, OtherApplication, Enabled, InvalidSetting, UnsupportedTargetShape };
constexpr Selection Select(std::string_view setting, std::uint64_t program_id,
                           bool is_application, bool is_39_bit) {
    if (setting.empty() || setting == "0") return Selection::Off;
    if (setting != "1") return Selection::InvalidSetting;
    if (program_id != ProgramId) return Selection::OtherApplication;
    if (!is_application || !is_39_bit) return Selection::UnsupportedTargetShape;
    return Selection::Enabled;
}
} // namespace Kernel::ResearchRemotePolicy
