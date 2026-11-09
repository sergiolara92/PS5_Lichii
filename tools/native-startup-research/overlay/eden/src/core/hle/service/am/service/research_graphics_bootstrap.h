// SPDX-License-Identifier: GPL-3.0-or-later
// Research subset of GSV lifecycle; no GPU/display/mapping support implied.
#pragma once
#include <cstdint>
#include <span>
namespace Service::AM::ResearchGraphicsBootstrap {
enum class Phase { Created, Initialized, ProcessBound, ContextCreated };
enum class Status { Success, InvalidPacket, InvalidState, InvalidProcess, InvalidAruid, Unsupported };
class State {
public:
    explicit State(std::uint64_t aruid) : aruid{aruid} {}
    Phase GetPhase() const { return phase; }
    Status Apply(std::uint32_t command, std::span<const std::uint32_t> p,
                 bool sender_matches, bool process_handle_matches, bool unbacked_reservation) {
        if (!sender_matches) return Status::InvalidProcess;
        switch (command) {
        case 4:
            if (p.size() < 16 || p[0] != 0x14 || p[1] != 0x8000000c || p[2] != 2)
                return Status::InvalidPacket;
            if (!process_handle_matches) return Status::InvalidProcess;
            if (phase != Phase::Created) return Status::InvalidState;
            if (Read64(p, 4) != aruid) return Status::InvalidAruid;
            // Only the version actually observed in this game's original SDK.
            if (Read64(p, 6) != 0x8000000000000001ULL || !unbacked_reservation)
                return Status::Unsupported;
            for (unsigned i = 8; i < 16; ++i) if (p[i] != 0) return Status::Unsupported;
            phase = Phase::Initialized;
            return Status::Success;
        case 11:
            if (p.size() < 4 || p[0] != 0x1b || p[1] != 0x80000000 || p[2] != 2)
                return Status::InvalidPacket;
            if (!process_handle_matches) return Status::InvalidProcess;
            if (phase != Phase::Initialized) return Status::InvalidState;
            phase = Phase::ProcessBound;
            return Status::Success;
        case 101:
            if (p.size() < 2 || p[0] != 0x75 || p[1] != 0) return Status::InvalidPacket;
            if (phase != Phase::ProcessBound) return Status::InvalidState;
            // Owns this per-process user session only; GPU/display contexts
            // have separate commands and remain unsupported.
            phase = Phase::ContextCreated;
            return Status::Success;
        default:
            return Status::Unsupported;
        }
    }
private:
    static std::uint64_t Read64(std::span<const std::uint32_t> p, unsigned i) {
        return std::uint64_t{p[i]} | (std::uint64_t{p[i + 1]} << 32);
    }
    std::uint64_t aruid;
    Phase phase{Phase::Created};
};
} // namespace Service::AM::ResearchGraphicsBootstrap
