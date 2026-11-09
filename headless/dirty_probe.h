// SPDX-License-Identifier: GPL-3.0-or-later
// PC diagnostics (EDEN_HOST_DIRTY_PROBE=1): guest memory written per second, from the kernel's
// soft-dirty page bits on the two mappings of the guest's physical memory (the backing, which
// services and the GPU write through, and the fastmem window the JIT writes through). It sizes a
// GPU command capture (coordination/notes/gpu-replay.md). Linux only; nothing on the console.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace Eden::DirtyProbe {

// Length of the run of adjacent mappings starting at `base` in /proc/self/maps (a backing
// allocated where used is a reservation with pieces mapped into it), or 0.
inline std::size_t MappingSize(const void* base) {
    std::ifstream maps("/proc/self/maps");
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    std::uintptr_t end = 0;
    for (std::string line; std::getline(maps, line);) {
        unsigned long long begin = 0, finish = 0;
        if (std::sscanf(line.c_str(), "%llx-%llx", &begin, &finish) != 2) continue;
        if (begin == start || (end != 0 && begin == end)) {
            end = finish;
        } else if (end != 0) {
            break;
        }
    }
    return end != 0 ? end - start : 0;
}

// Pages of [base, base + size) written since the last clear (pagemap bit 55).
inline std::uint64_t DirtyPages(int pagemap, const void* base, std::size_t size) {
    constexpr std::size_t Batch = 65536;
    std::vector<std::uint64_t> entries(Batch);
    const std::uint64_t first = reinterpret_cast<std::uintptr_t>(base) / 4096;
    const std::uint64_t pages = size / 4096;
    std::uint64_t dirty = 0;
    for (std::uint64_t done = 0; done < pages;) {
        const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(Batch, pages - done));
        const ssize_t got = pread(pagemap, entries.data(), count * 8, static_cast<off_t>((first + done) * 8));
        if (got <= 0) break;
        for (std::size_t i = 0; i < static_cast<std::size_t>(got) / 8; ++i) dirty += (entries[i] >> 55) & 1;
        done += static_cast<std::uint64_t>(got) / 8;
    }
    return dirty;
}

inline void Start(const void* backing, const void* fastmem) {
    std::thread([backing, fastmem] {
        const std::size_t backing_size = MappingSize(backing), fastmem_size = MappingSize(fastmem);
        const int pagemap = open("/proc/self/pagemap", O_RDONLY);
        std::fprintf(stderr, "EDEN_DIRTY start backing=%zu MiB fastmem=%zu MiB pagemap=%d\n",
                     backing_size >> 20, fastmem_size >> 20, pagemap);
        if (pagemap < 0 || backing_size == 0) return;
        const auto clear = [] { std::ofstream("/proc/self/clear_refs") << "4"; };
        clear();
        for (auto last = std::chrono::steady_clock::now();;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const auto now = std::chrono::steady_clock::now();
            const std::uint64_t b = DirtyPages(pagemap, backing, backing_size);
            const std::uint64_t f = fastmem_size ? DirtyPages(pagemap, fastmem, fastmem_size) : 0;
            clear();
            const double seconds = std::chrono::duration<double>(now - last).count();
            last = now;
            std::fprintf(stderr, "EDEN_DIRTY mono_ns=%lld backing_mib_s=%.1f fastmem_mib_s=%.1f\n",
                         static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             now.time_since_epoch()).count()),
                         b * 4096.0 / seconds / 1048576.0, f * 4096.0 / seconds / 1048576.0);
        }
    }).detach();
}

} // namespace Eden::DirtyProbe
