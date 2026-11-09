// SPDX-License-Identifier: GPL-3.0-or-later
// Explicit native diagnostic route policy; no authentication or retail-capacity claim.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string_view>
namespace Core::NativeStartupResearch {
inline constexpr std::uint64_t ProgramId = 0x0400c3f00006e000ULL;
struct Profiles { bool valid = true; bool active = false; bool pauth = false; bool remote = false; };
inline std::string_view Environment(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string_view(value) : std::string_view{};
}
inline Profiles ReadSelection() {
    const auto active = Environment("PROSPERO_RESEARCH_NATIVE_ROUTE_ACTIVE");
    if (active.empty() || active == "0") return {};
    if (active != "1") return {false};
    const auto pauth = Environment("PROSPERO_RESEARCH_PAUTH_DISABLED_PROFILE");
    const auto remote = Environment("PROSPERO_RESEARCH_REMOTE_REGION_PROFILE");
    const bool valid = (pauth.empty() || pauth == "0" || pauth == "1") &&
                       (remote.empty() || remote == "0" || remote == "1");
    return {valid, true, pauth == "1", remote == "1"};
}
enum class TargetSelection { Off, Enabled, UnsupportedTargetShape };
constexpr TargetSelection SelectPAuth(Profiles selection, std::uint64_t program_id,
                                     bool is_application, bool is_64_bit, bool is_39_bit) {
    if (!selection.active || !selection.pauth || program_id != ProgramId)
        return TargetSelection::Off;
    if (!is_application || !is_64_bit || !is_39_bit)
        return TargetSelection::UnsupportedTargetShape;
    return TargetSelection::Enabled;
}
constexpr bool IsFailClosedTarget(Profiles selection, std::uint64_t program_id,
                                  bool is_application, bool is_64_bit, bool is_39_bit) {
    return selection.active && program_id == ProgramId && is_application && is_64_bit && is_39_bit;
}
[[noreturn]] inline void AbortUnsupportedEncoding(std::uint64_t pc, std::uint32_t opcode,
                                                 bool code_valid) {
    std::fprintf(stderr, "NATIVE_RESEARCH unallocated_encoding pc=%016llx opcode=%08x code_valid=%u action=abort_no_fallthrough\n",
                 static_cast<unsigned long long>(pc), opcode, code_valid);
    std::fflush(stderr);
    std::abort(); // Explicit research-only fallback; no proved graceful stop is assumed.
}
} // namespace Core::NativeStartupResearch
