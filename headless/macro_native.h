// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Native versions of Mario Kart World's hottest Turing macros (title 1's GPU thread runs ~400,000
// macro calls a second in races; most are a few method writes, and the JIT's per-call setup was
// most of their cost). Each is matched by the exact code words the game uploaded at the call's
// start, so another game's (or a changed) macro never matches. A body checks everything that
// could make the macro fail (parameters missing, data RAM out of bounds, loops past a bound)
// before its first effect and then returns false: the JIT runs the call. Otherwise its method
// writes and data RAM writes are the macro's, in the same order (macro_native_check.cpp checks
// them against TuringMacroInterpreter; dev setting mme_native=check does in a game).
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "video_core/macro_turing.h"

namespace Eden::MacroNative {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

// The code each replacement stands for, 3 words an instruction from its start, as uploaded.
// Start addresses are where title 1 (0400C3F00006E000, 1.0) puts them; matching is by content.
inline constexpr u32 kConstBufferSelect[] = { // 0x75
    0x0000130f, 0x1907812f, 0x39908300, 0x00000003, 0x18c00000, 0x3010fb00,
    0x10586383, 0x00c00046, 0x038c4301, 0x00200003, 0x18c008c0, 0x038c4300,
};
inline constexpr u32 kConstBufferBind[] = { // 0x21
    0x00000003, 0x1d200000, 0x31c28300, 0x00000003, 0x1d100000, 0x31c18300,
    0x00000023, 0x18c02001, 0xf45c6700, 0x00000003, 0x18c00128, 0x0b900300,
    0x00001003, 0x18c00510, 0x0ba0c300, 0x00000003, 0x18c00120, 0x0810c300,
    0x00000003, 0x002f8000, 0x72100300, 0x00000003, 0x18c00000, 0x061c7f00,
    0x00000003, 0x18c00000, 0x040c7f00, 0x0000003f, 0x18c00400, 0x345c6700,
    0x00386383, 0x18c000a3, 0x058c4300, 0x20200003, 0x03c00040, 0x058c4300,
    0x00000003, 0x18c00000, 0xf2400300, 0x00042413, 0x18c00318, 0x0bac4301,
    0x00200007, 0x18c004e0, 0x01ac4300,
};
inline constexpr u32 kShaderBind[] = { // 0x8
    0x00000003, 0x5c110001, 0x31c00300, 0x00000003, 0x1c400120, 0x32228300,
    0x00000000, 0x1a0a4000, 0x70507b00, 0x00000003, 0x18c02400, 0x008c7700,
    0x00001207, 0x2220012b, 0x31907b00, 0x0000120f, 0x22100120, 0xb2218300,
    0x00000003, 0x1d300000, 0x31c38300, 0x00000001, 0x05c7c000, 0x31c28300,
    0x00000003, 0x18c00000, 0x0e4c7f00, 0x00000003, 0x18c00000, 0x0c3c7f00,
    0x00000000, 0x20030000, 0x0a2c7f00, 0x00000003, 0x18c02400, 0x300c7700,
    0x00046017, 0x18c0041e, 0x0bac4300, 0x20208c00, 0xf8c40046, 0x0f8c4300,
    0x0c206383, 0x18c0003a, 0x4b8c4300, 0x01400003, 0x18c00000, 0x718c0300,
    0x20208c00, 0xd8c40046, 0x0d8c4300, 0x00042413, 0x18c0031f, 0x0bac4300,
    0x00a00003, 0x18c00004, 0x718c0300, 0x0004200f, 0x18c0041e, 0x0bac4300,
    0x00200003, 0x18c00048, 0x0b8c4300, 0x00042003, 0x18c0041e, 0x0bac4301,
    0x00200007, 0x18c0041e, 0x0bac4300,
};
inline constexpr u32 kShaderUnbind[] = { // 0x188
    0x00000003, 0x1c18012b, 0x31907b00, 0x00000003, 0x18c00020, 0x0780c300,
    0x00000003, 0x18c00001, 0x32111300, 0x00000004, 0x390a4120, 0x32210300,
    0x00000003, 0x02c7c000, 0x02003300, 0x00042003, 0x18c00419, 0x07ac4005,
    0x00200003, 0x18c00419, 0x078c4004,
};
inline constexpr u32 kVertexFormats[] = { // 0x319
    0x00000003, 0x1a0f8133, 0x31c10300, 0x00000103, 0x18c00e90, 0x31b00300,
    0x00185160, 0x41088000, 0xb02c5f00, 0x40600003, 0x18c00000, 0x318c0300,
    0x00001330, 0x59c7e400, 0x301c6f00, 0x00000003, 0x18c00000, 0xb01c5f00,
    0x20200003, 0x00c00000, 0x300c0300, 0x00000003, 0x18c00000, 0x318c0301,
    0x00000003, 0x18c00000, 0x318c0300,
};

// The same macros from another driver build keep their tables 0x80 data RAM words further on
// (Ocarina of Time, 0400486024DEC000: 0x480 -> 0x500, 0x4bc -> 0x53c): the words that hold a table
// address carry 0x20 more, in an immediate at bit 0 (scale 1) or bit 4 (scale 16).
struct Reloc {
    std::uint8_t word;
    std::uint8_t scale;
};
inline constexpr Reloc kConstBufferSelectRelocs[] = {{0, 16}, {1, 1}};
inline constexpr Reloc kConstBufferBindRelocs[] = {{10, 1}, {12, 16}};
inline constexpr Reloc kShaderBindRelocs[] = {{4, 1}, {12, 16}, {13, 1}, {15, 16}, {16, 1}};
inline constexpr u32 kShiftedTables = 0x20; // in the immediates; 0x80 data RAM words

enum Id : std::uint8_t {
    None, ConstBufferSelect, ConstBufferBind, ShaderBind, ShaderUnbind, VertexFormats,
    ConstBufferSelectShifted, ConstBufferBindShifted, ShaderBindShifted, Count
};

inline constexpr std::array<std::span<const u32>, Count> kCode{
    std::span<const u32>{}, kConstBufferSelect, kConstBufferBind, kShaderBind, kShaderUnbind, kVertexFormats,
    kConstBufferSelect, kConstBufferBind, kShaderBind,
};
inline constexpr std::array<std::span<const Reloc>, Count> kRelocs{
    std::span<const Reloc>{}, {}, {}, {}, {}, {},
    kConstBufferSelectRelocs, kConstBufferBindRelocs, kShaderBindRelocs,
};
inline constexpr std::array<const char*, Count> kNames{
    "none", "const buffer select", "const buffer bind", "shader bind", "shader unbind", "vertex formats",
    "const buffer select (tables +0x80)", "const buffer bind (tables +0x80)", "shader bind (tables +0x80)",
};

/// The code replacement `id` stands for (the shifted ones with their table addresses moved).
inline std::vector<u32> Code(Id id) {
    std::vector<u32> words(kCode[id].begin(), kCode[id].end());
    for (const Reloc reloc : kRelocs[id]) {
        words[reloc.word] += kShiftedTables * reloc.scale;
    }
    return words;
}

inline Id Match(std::span<const u32> code, u32 start) {
    for (std::uint8_t id = 1; id < Count; ++id) {
        const auto words = kCode[id];
        if (std::size_t{start} * 3 + words.size() > code.size()) {
            continue;
        }
        const auto at = code.begin() + std::size_t{start} * 3;
        bool same = true;
        for (std::size_t i = 0; i < words.size() && same; ++i) {
            u32 expected = words[i];
            for (const Reloc reloc : kRelocs[id]) {
                expected += reloc.word == i ? kShiftedTables * reloc.scale : 0;
            }
            same = at[i] == expected;
        }
        if (same) {
            return static_cast<Id>(id);
        }
    }
    return None;
}

// Which replacement runs each start of a code RAM, until its code generation changes.
inline Id Find(std::span<const u32> code, u64 generation, u32 start) {
    struct Matches {
        const u32* code{};
        u64 generation{~u64{}};
        std::array<std::uint8_t, 0x1000> ids{}; // 0xff: not looked at yet
    };
    // Plain statics: only one thread runs the macros at a time (the GPU thread, or the split's
    // front or back), and a thread_local here cost ~1.8% of the GPU thread on the console, where
    // every access goes through emulated TLS (__emutls_get_address).
    static std::array<Matches, 4> caches{};
    static std::size_t next{};
    if (start >= 0x1000) return None;
    Matches* matches = nullptr;
    for (auto& cache : caches) {
        if (cache.code == code.data()) matches = &cache;
    }
    if (matches == nullptr) {
        matches = &caches[next++ % caches.size()];
        matches->code = code.data();
        matches->generation = ~u64{};
    }
    if (matches->generation != generation) {
        matches->generation = generation;
        matches->ids.fill(0xff);
    }
    auto& id = matches->ids[start];
    if (id == 0xff) id = Match(code, start);
    return static_cast<Id>(id);
}

// 0x75 (1 parameter): the constant buffer selected for data uploads (size, address) from the
// data RAM table at 0x4bc.
template <typename Write>
bool RunConstBufferSelect(std::span<const u32> p, std::span<u32> dram, Write& write, u32 t = 0) {
    if (p.size() < 1) return false;
    const u32 index = 0x4bc + t + p[0];
    if (index >= dram.size()) return false;
    const u32 entry = dram[index];
    write(0x8e0, dram[0x4c3 + t]);
    write(0x8e1, entry >> 24);
    write(0x8e2, (entry & 0xffffff) << 8);
    return true;
}

// 0x21 (stage or slot, index, address low, size and address high): binds a constant buffer to a
// stage; from slot 8 on, the stage comes from the data RAM table at 0x4a0 and the buffer is kept
// in the table at 0x400.
template <typename Write>
bool RunConstBufferBind(std::span<const u32> p, std::span<u32> dram, Write& write, u32 t = 0) {
    if (p.size() < 4) return false;
    u32 stage = p[0];
    if (!(static_cast<s32>(p[0]) < 8)) {
        const u32 table = 0x4a0 + t + p[0];
        if (table >= dram.size()) return false;
        const u32 slot = (0x400 + t) | (p[0] & 3) << 5 | (p[1] & 15) << 1;
        stage = dram[table];
        dram[slot] = p[2];
        dram[slot + 1] = p[3];
    }
    if (!(static_cast<s32>(stage) < 15)) return true;
    write(0x8e0, (p[3] >> 12) & 0xfffff);
    write(0x8e1, p[3] & 0xff);
    write(0x8e2, p[2]);
    write(0x904 + (stage & 7) * 8, 1 | ((p[1] + 3) & 0xfffffff) << 4);
    return true;
}

// 0x8 (slot, program offset, constant buffer address, address, configuration): a shader stage's
// program and its constant buffer 1; nothing when the data RAM table at 0x480 already has it,
// writes only when the slot is enabled (data RAM 0x4ac).
template <typename Write>
bool RunShaderBind(std::span<const u32> p, std::span<u32> dram, Write& write, u32 t = 0) {
    if (p.size() < 5) return false;
    const u32 base = 0x480 + t + p[0] * 4;
    if (base >= dram.size()) return false;
    if (p[1] == dram[base]) return true;
    const u32 enabled = dram[0x4ac + t];
    dram[base] = p[1];
    dram[base + 1] = p[2];
    dram[base + 2] = p[3];
    dram[base + 3] = p[4];
    if ((enabled & (1U << (p[0] & 31))) == 0) return true;
    const u32 program = ((p[4] >> 24) & 7) * 16, bind = ((p[4] >> 28) & 7) * 8;
    write(0x805 + program, p[2] >> 24);
    write(0x806 + program, (p[2] & 0xffffff) << 8);
    write(0x807 + program, (p[4] >> 9) & 0x7f);
    write(0x8e0, 0x10000);
    write(0x8e1, p[3] >> 24);
    write(0x8e2, (p[3] & 0xffffff) << 8);
    write(0x904 + bind, 0x11);
    write(0x803 + program, p[4] & 0x1ff);
    write(0x800 + program, 1 | program);
    return true;
}

// 0x188 (slot and program): forgets the slot's program in the table at 0x480 and turns the
// program off when the slot is enabled.
template <typename Write>
bool RunShaderUnbind(std::span<const u32> p, std::span<u32> dram, Write& write) {
    if (p.size() < 1) return false;
    const u32 slot = p[0] & 15, program = ((p[0] >> 4) & 7) * 16;
    const u32 enabled = dram[0x4ac];
    dram[0x480 + slot * 4] = 0;
    if (enabled & (1U << slot)) write(0x800 + program, program);
    return true;
}

// 0x319 (count, then count pairs): vertex attribute formats from 0x458; the ones the previous
// call set beyond count (data RAM 0x4cc) get 0x3a400040.
template <typename Write>
bool RunVertexFormats(std::span<const u32> p, std::span<u32> dram, Write& write) {
    // shortcut: up to 64 pairs either way (the hardware has 32), the JIT runs anything larger.
    if (p.size() < 1 || p[0] > 64 || p.size() < 1 + std::size_t{p[0]} * 2) return false;
    const u32 count = p[0], rest = dram[0x4cc] - count;
    if (static_cast<s32>(rest) > 64) return false;
    u32 method = 0x458;
    for (u32 i = 0; i < count * 2; ++i) write(method++, p[1 + i]);
    dram[0x4cc] = count;
    if (static_cast<s32>(rest) <= 0) return true;
    for (u32 i = 0; i < rest * 2; ++i) write(method++, 0x3a400040);
    return true;
}

// Runs replacement `id`; false: the macro's own code must run the call (nothing was done).
template <typename Write>
bool Run(Id id, std::span<const u32> parameters, std::span<u32> dram, Write&& write) {
    switch (id) {
    case ConstBufferSelect: return RunConstBufferSelect(parameters, dram, write);
    case ConstBufferBind: return RunConstBufferBind(parameters, dram, write);
    case ShaderBind: return RunShaderBind(parameters, dram, write);
    case ShaderUnbind: return RunShaderUnbind(parameters, dram, write);
    case VertexFormats: return RunVertexFormats(parameters, dram, write);
    case ConstBufferSelectShifted: return RunConstBufferSelect(parameters, dram, write, 4 * kShiftedTables);
    case ConstBufferBindShifted: return RunConstBufferBind(parameters, dram, write, 4 * kShiftedTables);
    case ShaderBindShifted: return RunShaderBind(parameters, dram, write, 4 * kShiftedTables);
    default: return false;
    }
}

// Runs replacement `id` and the interpreter on copies of the data RAM: "" when the replacement
// declines the call or both make the same method writes and leave the same data RAM; otherwise
// what differs. `read` is MME_READ of a register (none of the replacements reads one).
template <typename Read>
std::string Compare(Id id, std::span<const u32> code, u32 start, std::span<const u32> parameters,
                    std::span<const u32> dram, Read read) {
    using Interpreter = Tegra::TuringMacroInterpreter;
    Interpreter::DataRam native_dram{}, reference_dram{};
    std::copy_n(dram.begin(), std::min(dram.size(), native_dram.size()), native_dram.begin());
    reference_dram = native_dram;
    std::vector<std::pair<u32, u32>> native_writes, reference_writes;
    if (!Run(id, parameters, native_dram, [&](u32 method, u32 value) { native_writes.emplace_back(method, value); })) {
        return {};
    }
    Interpreter interpreter;
    interpreter.active_dram = &reference_dram;
    std::vector<u32> fifo(parameters.begin(), parameters.end());
    const auto result = interpreter.Execute(code, start, fifo, read, [&](u32 method, u32 value) {
        reference_writes.emplace_back(method, value);
        return method < 0xe00;
    });
    std::string difference;
    if (!result.success) {
        difference = "the macro fails (" + std::string{result.error} + ")";
    } else if (native_writes != reference_writes) {
        difference = "writes differ:";
        for (std::size_t i = 0; i < std::max(native_writes.size(), reference_writes.size()); ++i) {
            const auto show = [](const auto& writes, std::size_t i) {
                if (i >= writes.size()) return std::string{"-"};
                char text[32];
                std::snprintf(text, sizeof(text), "%x=%x", writes[i].first, writes[i].second);
                return std::string{text};
            };
            difference += " " + show(native_writes, i) + "/" + show(reference_writes, i);
        }
    } else if (native_dram != reference_dram) {
        const auto at = std::mismatch(native_dram.begin(), native_dram.end(), reference_dram.begin()).first -
                        native_dram.begin();
        char text[64];
        std::snprintf(text, sizeof(text), "data RAM differs at %#zx: %x/%x", static_cast<std::size_t>(at),
                      native_dram[at], reference_dram[at]);
        difference = text;
    }
    return difference;
}

} // namespace Eden::MacroNative
