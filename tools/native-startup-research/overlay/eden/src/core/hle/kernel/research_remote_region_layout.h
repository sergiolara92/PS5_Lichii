// Experimental HLE virtual-region placement policy, not a retail layout claim.
// This standalone helper allocates no physical memory and is OFF unless called.
#pragma once
#include <array>
#include <cstdint>
#include <limits>

namespace Kernel::ResearchRemoteLayout {
constexpr std::uint64_t Alignment = 2ULL * 1024 * 1024;
struct Region {
    std::uint64_t start;
    std::uint64_t size;
    std::uint64_t random_offset;
};

// Legacy regions must already be packed by the unchanged four-region algorithm,
// using remaining = alloc_size - legacy_sizes - remote_size. Equal offsets put
// the remote region after all legacy regions, preserving their existing ties.
inline bool Insert(std::array<Region, 4>& legacy, std::uint64_t alloc_start,
                   std::uint64_t alloc_size, std::uint64_t remote_size,
                   std::uint64_t remote_offset, std::uint64_t& remote_start) {
    if (!remote_size || remote_size % Alignment || alloc_start % Alignment ||
        remote_offset % Alignment || alloc_start > std::numeric_limits<std::uint64_t>::max() - alloc_size)
        return false;
    const auto alloc_end = alloc_start + alloc_size;
    std::uint64_t needed = remote_size;
    for (const auto& region : legacy) {
        if (region.size % Alignment || region.start % Alignment || region.random_offset % Alignment ||
            region.size > alloc_size || needed > alloc_size - region.size)
            return false;
        needed += region.size;
    }
    if (needed > alloc_size || remote_offset > alloc_size - needed)
        return false;
    auto candidate = legacy; // Invalid inputs never mutate callers' tracked ranges.
    std::uint64_t result = alloc_start + remote_offset;
    for (auto& region : candidate) {
        if (region.random_offset > alloc_size - needed || region.start < alloc_start ||
            region.start > alloc_end || region.size > alloc_end - region.start)
            return false;
        if (region.random_offset <= remote_offset) {
            if (result > alloc_end - region.size) return false;
            result += region.size;
        } else {
            if (region.start > alloc_end - remote_size) return false;
            region.start += remote_size;
        }
    }
    if (result > alloc_end || remote_size > alloc_end - result) return false;
    auto overlaps = [](std::uint64_t a, std::uint64_t as, std::uint64_t b, std::uint64_t bs) {
        return as && bs && a < b + bs && b < a + as;
    };
    for (std::size_t i = 0; i < candidate.size(); ++i) {
        const auto& a = candidate[i];
        if (a.start > alloc_end || a.size > alloc_end - a.start ||
            overlaps(a.start, a.size, result, remote_size)) return false;
        for (std::size_t j = i + 1; j < candidate.size(); ++j)
            if (overlaps(a.start, a.size, candidate[j].start, candidate[j].size)) return false;
    }
    legacy = candidate;
    remote_start = result;
    return true;
}
} // namespace Kernel::ResearchRemoteLayout
