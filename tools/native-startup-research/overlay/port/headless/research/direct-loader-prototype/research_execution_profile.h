// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace Prospero::Development {
inline constexpr std::uint64_t StrictMemoryProgramId = 0x0400c3f00006e000ULL;
constexpr bool SelectStrictMemoryProfile(bool requested, bool execute, std::uint64_t program_id,
                                        bool is_64_bit, bool is_39_bit, bool is_application) {
    return requested && execute && program_id == StrictMemoryProgramId &&
           is_64_bit && is_39_bit && is_application;
}
} // namespace Prospero::Development
