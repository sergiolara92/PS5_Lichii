// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// EDEN_NX2_MME_CENSUS (derived macro.cpp, GPU thread, split off): per Turing macro start address,
// calls, wall time, and the part of it spent in the method writes the macro makes (with the
// draws they start); every 10 s the top starts are logged. EDEN_NX2_MME_SAMPLE=<start>,...
// logs the first 32 calls of those starts: parameters and method writes.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include "common/cpu_features.h"
#include "common/logging.h"

namespace Eden::MacroCensus {

// Read at the first macro call: on the console, dev-settings set it after static initialization.
// A plain variable, not a function-local static: the check runs on every macro write, and on the
// console the static's guard cost ~1.6% of the GPU thread with the census off.
inline int g_enabled = -1;
inline bool Enabled() {
    if (g_enabled < 0) [[unlikely]] g_enabled = std::getenv("EDEN_NX2_MME_CENSUS") != nullptr;
    return g_enabled != 0;
}

struct Entry {
    std::uint64_t calls, ns, emit_ns, emits, params;
};

struct Data {
    std::array<Entry, 0x1000> entries{};
    std::array<std::uint8_t, 0x1000> sampled{}; // calls of a sampled start logged so far
    std::array<bool, 0x1000> dumped{};          // code logged once per start
    std::span<const std::uint32_t> code;        // the macro code RAM (3 words an instruction)
    std::uint64_t window_start{};
    std::uint64_t emit_ns{}, emits{};           // of the call in progress
    int depth{};
    bool sampling{};
    std::string sample;
};

inline thread_local std::unique_ptr<Data> t_data;

inline std::uint64_t Now() {
    return static_cast<std::uint64_t>(Common::g_wall_clock.GetTimeNS().count());
}

inline bool IsSampled(std::uint32_t start) {
    static const std::vector<std::uint32_t> starts = [] {
        std::vector<std::uint32_t> list;
        if (const char* value = std::getenv("EDEN_NX2_MME_SAMPLE")) {
            for (char* end = nullptr;; value = end + 1) {
                list.push_back(static_cast<std::uint32_t>(std::strtoul(value, &end, 0)));
                if (*end != ',') break;
            }
        }
        return list;
    }();
    return std::ranges::find(starts, start) != starts.end();
}

inline void Report(Data& data, std::uint64_t now) {
    if (data.window_start == 0) {
        data.window_start = now;
        return;
    }
    const std::uint64_t window = now - data.window_start;
    if (window < 10'000'000'000ULL) return;
    std::vector<std::uint32_t> order;
    std::uint64_t own{}, emit{};
    for (std::uint32_t i = 0; i < data.entries.size(); ++i) {
        if (data.entries[i].calls == 0) continue;
        order.push_back(i);
        own += data.entries[i].ns - data.entries[i].emit_ns;
        emit += data.entries[i].emit_ns;
    }
    std::ranges::sort(order, [&](auto a, auto b) { return data.entries[a].ns > data.entries[b].ns; });
    std::string line = fmt::format("MME census per s: own {:.1f} ms, writes {:.1f} ms |",
                                   own * 1e3 / window, emit * 1e3 / window);
    for (std::size_t k = 0; k < std::min<std::size_t>(order.size(), 16); ++k) {
        const auto& e = data.entries[order[k]];
        line += fmt::format(" {:#x}: {:.0f}/s own {:.2f} ms writes {:.2f} ms ({:.1f}/call, {:.1f} params) |",
                            order[k], e.calls * 1e9 / window, (e.ns - e.emit_ns) * 1e3 / window,
                            e.emit_ns * 1e3 / window, double(e.emits) / e.calls,
                            double(e.params) / e.calls);
    }
    LOG_INFO(HW_GPU, "{}", line);
    // The code of each hot start once: 96 instructions from it (the macro and what follows).
    for (std::size_t k = 0; k < std::min<std::size_t>(order.size(), 16); ++k) {
        const std::uint32_t start = order[k];
        if (data.dumped[start] || data.code.size() < (start + 1) * 3) continue;
        data.dumped[start] = true;
        const auto words = data.code.subspan(start * 3, std::min<std::size_t>(data.code.size() - start * 3, 96 * 3));
        LOG_INFO(HW_GPU, "MME code {:#x}: {:08x}", start, fmt::join(words, ","));
    }
    data.entries = {};
    data.window_start = now;
}

// Around one macro call (MacroEngine::Execute).
class Call {
public:
    Call(std::uint32_t start_, std::span<const std::uint32_t> parameters, std::span<const std::uint32_t> code)
        : start{start_ & 0xfff} {
        if (!Enabled()) return;
        if (!t_data) t_data = std::make_unique<Data>();
        auto& data = *t_data;
        data.code = code;
        if (data.depth++ != 0) return;
        data.emit_ns = data.emits = 0;
        data.sampling = data.sampled[start] < 32 && IsSampled(start);
        if (data.sampling) {
            ++data.sampled[start];
            data.sample = fmt::format("MME sample {:#x} params={:x} writes=", start, fmt::join(parameters, ","));
        }
        params = parameters.size();
        begin = Now();
    }
    ~Call() {
        if (!Enabled() || --t_data->depth != 0) return;
        auto& data = *t_data;
        const std::uint64_t now = Now();
        auto& e = data.entries[start];
        ++e.calls;
        e.ns += now - begin;
        e.emit_ns += data.emit_ns;
        e.emits += data.emits;
        e.params += params;
        if (data.sampling) LOG_INFO(HW_GPU, "{}", data.sample);
        Report(data, now);
    }
    Call(const Call&) = delete;
    Call& operator=(const Call&) = delete;

private:
    std::uint32_t start;
    std::uint64_t begin{}, params{};
};

// Around one method write of a macro.
class Emit {
public:
    Emit(std::uint32_t address, std::uint32_t value) {
        if (!Enabled() || !t_data) return;
        if (t_data->sampling) t_data->sample += fmt::format("{:x}={:x},", address, value);
        begin = Now();
    }
    ~Emit() {
        if (begin == 0) return;
        t_data->emit_ns += Now() - begin;
        ++t_data->emits;
    }
    Emit(const Emit&) = delete;
    Emit& operator=(const Emit&) = delete;

private:
    std::uint64_t begin{};
};

} // namespace Eden::MacroCensus
