// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <array>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <tuple>
#include <utility>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include "dynarmic/mcl/integer_of_size.hpp"
#include "dynarmic/backend/x64/xbyak.h"

#include "dynarmic/backend/x64/a64_emit_x64.h"
#include "dynarmic/backend/x64/abi.h"
#include "dynarmic/backend/x64/devirtualize.h"
#include "dynarmic/backend/x64/emit_x64_memory.h"
#include "dynarmic/backend/x64/exclusive_monitor_friend.h"
#include "dynarmic/backend/x64/perf_map.h"
#include "dynarmic/common/spin_lock_x64.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/common/atomic.h"

namespace Dynarmic::Backend::X64 {

using namespace Xbyak::util;

void A64EmitX64::GenMemory128Accessors() {
    code.align();
    memory_read_128 = code.getCurr<void (*)()>();
#ifdef _WIN32
    Devirtualize<&A64::UserCallbacks::MemoryRead128>(conf.callbacks).EmitCallWithReturnPointer(code, [&](Xbyak::Reg64 return_value_ptr, [[maybe_unused]] RegList args) {
        code.mov(code.ABI_PARAM3, code.ABI_PARAM2);
        code.lea(rsp, ptr[rsp - (8 + 16 + ABI_SHADOW_SPACE)]);
        code.lea(return_value_ptr, ptr[rsp + ABI_SHADOW_SPACE]);
    });
    code.movups(xmm1, xword[code.ABI_RETURN]);
    code.add(rsp, 8 + 16 + ABI_SHADOW_SPACE);
#else
    code.lea(rsp, ptr[rsp - 8]);
    Devirtualize<&A64::UserCallbacks::MemoryRead128>(conf.callbacks).EmitCall(code);
    if (code.HasHostFeature(HostFeature::SSE41)) {
        code.movq(xmm1, code.ABI_RETURN);
        code.pinsrq(xmm1, code.ABI_RETURN2, 1);
    } else {
        code.movq(xmm1, code.ABI_RETURN);
        code.movq(xmm2, code.ABI_RETURN2);
        code.punpcklqdq(xmm1, xmm2);
    }
    code.add(rsp, 8);
#endif
    code.ret();
    PerfMapRegister(memory_read_128, code.getCurr(), "a64_memory_read_128");

    code.align();
    memory_write_128 = code.getCurr<void (*)()>();
#ifdef _WIN32
    code.lea(rsp, ptr[rsp - (8 + 16 + ABI_SHADOW_SPACE)]);
    code.lea(code.ABI_PARAM3, ptr[rsp + ABI_SHADOW_SPACE]);
    code.movaps(xword[code.ABI_PARAM3], xmm1);
    Devirtualize<&A64::UserCallbacks::MemoryWrite128>(conf.callbacks).EmitCall(code);
    code.add(rsp, 8 + 16 + ABI_SHADOW_SPACE);
#else
    code.lea(rsp, ptr[rsp - 8]);
    if (code.HasHostFeature(HostFeature::SSE41)) {
        code.movq(code.ABI_PARAM3, xmm1);
        code.pextrq(code.ABI_PARAM4, xmm1, 1);
    } else {
        code.movq(code.ABI_PARAM3, xmm1);
        code.punpckhqdq(xmm1, xmm1);
        code.movq(code.ABI_PARAM4, xmm1);
    }
    Devirtualize<&A64::UserCallbacks::MemoryWrite128>(conf.callbacks).EmitCall(code);
    code.add(rsp, 8);
#endif
    code.ret();
    PerfMapRegister(memory_write_128, code.getCurr(), "a64_memory_write_128");

    code.align();
    memory_exclusive_write_128 = code.getCurr<void (*)()>();
#ifdef _WIN32
    code.lea(rsp, ptr[rsp - (8 + 32 + ABI_SHADOW_SPACE)]);
    code.lea(code.ABI_PARAM3, ptr[rsp + ABI_SHADOW_SPACE]);
    code.lea(code.ABI_PARAM4, ptr[rsp + ABI_SHADOW_SPACE + 16]);
    code.movaps(xword[code.ABI_PARAM3], xmm1);
    code.movaps(xword[code.ABI_PARAM4], xmm2);
    Devirtualize<&A64::UserCallbacks::MemoryWriteExclusive128>(conf.callbacks).EmitCall(code);
    code.add(rsp, 8 + 32 + ABI_SHADOW_SPACE);
#else
    code.lea(rsp, ptr[rsp - 8]);
    if (code.HasHostFeature(HostFeature::SSE41)) {
        code.movq(code.ABI_PARAM3, xmm1);
        code.pextrq(code.ABI_PARAM4, xmm1, 1);
        code.movq(code.ABI_PARAM5, xmm2);
        code.pextrq(code.ABI_PARAM6, xmm2, 1);
    } else {
        code.movq(code.ABI_PARAM3, xmm1);
        code.punpckhqdq(xmm1, xmm1);
        code.movq(code.ABI_PARAM4, xmm1);
        code.movq(code.ABI_PARAM5, xmm2);
        code.punpckhqdq(xmm2, xmm2);
        code.movq(code.ABI_PARAM6, xmm2);
    }
    Devirtualize<&A64::UserCallbacks::MemoryWriteExclusive128>(conf.callbacks).EmitCall(code);
    code.add(rsp, 8);
#endif
    code.ret();
    PerfMapRegister(memory_exclusive_write_128, code.getCurr(), "a64_memory_exclusive_write_128");
}

void A64EmitX64::GenFastmemFallbacks() {
    const std::initializer_list<int> idxes{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const std::array<std::pair<size_t, ArgCallback>, 4> read_callbacks{{
        {8, Devirtualize<&A64::UserCallbacks::MemoryRead>(conf.callbacks)},
        {16, Devirtualize<&A64::UserCallbacks::MemoryRead>(conf.callbacks)},
        {32, Devirtualize<&A64::UserCallbacks::MemoryRead>(conf.callbacks)},
        {64, Devirtualize<&A64::UserCallbacks::MemoryRead>(conf.callbacks)},
    }};
    const std::array<std::pair<size_t, ArgCallback>, 4> write_callbacks{{
        {8, Devirtualize<&A64::UserCallbacks::MemoryWrite>(conf.callbacks)},
        {16, Devirtualize<&A64::UserCallbacks::MemoryWrite>(conf.callbacks)},
        {32, Devirtualize<&A64::UserCallbacks::MemoryWrite>(conf.callbacks)},
        {64, Devirtualize<&A64::UserCallbacks::MemoryWrite>(conf.callbacks)},
    }};
    const std::array<std::pair<size_t, ArgCallback>, 4> exclusive_write_callbacks{{
        {8, Devirtualize<&A64::UserCallbacks::MemoryWriteExclusive8>(conf.callbacks)},
        {16, Devirtualize<&A64::UserCallbacks::MemoryWriteExclusive16>(conf.callbacks)},
        {32, Devirtualize<&A64::UserCallbacks::MemoryWriteExclusive32>(conf.callbacks)},
        {64, Devirtualize<&A64::UserCallbacks::MemoryWriteExclusive64>(conf.callbacks)},
    }};

    for (auto const ordered : {false, true}) {
        for (auto const vaddr_idx : idxes) {
            if (vaddr_idx == 4 || vaddr_idx == 15) {
                continue;
            }

            for (int value_idx : idxes) {
                code.align();
                read_fallbacks[std::make_tuple(ordered, 128, vaddr_idx, value_idx)] = code.getCurr<void (*)()>();
                ABI_PushCallerSaveRegistersAndAdjustStackExcept(code, HostLocXmmIdx(value_idx));
                if (vaddr_idx != code.ABI_PARAM2.getIdx()) {
                    code.mov(code.ABI_PARAM2, Xbyak::Reg64{vaddr_idx});
                }
                if (ordered) {
                    code.mfence();
                }
                code.call(memory_read_128);
                if (value_idx != 1) {
                    code.movaps(Xbyak::Xmm{value_idx}, xmm1);
                }
                ABI_PopCallerSaveRegistersAndAdjustStackExcept(code, HostLocXmmIdx(value_idx));
                code.ret();
                PerfMapRegister(read_fallbacks[std::make_tuple(ordered, 128, vaddr_idx, value_idx)], code.getCurr(), "a64_read_fallback_128");

                code.align();
                write_fallbacks[std::make_tuple(ordered, 128, vaddr_idx, value_idx)] = code.getCurr<void (*)()>();
                ABI_PushCallerSaveRegistersAndAdjustStack(code);
                if (vaddr_idx != code.ABI_PARAM2.getIdx()) {
                    code.mov(code.ABI_PARAM2, Xbyak::Reg64{vaddr_idx});
                }
                if (value_idx != 1) {
                    code.movaps(xmm1, Xbyak::Xmm{value_idx});
                }
                code.call(memory_write_128);
                if (ordered) {
                    code.mfence();
                }
                ABI_PopCallerSaveRegistersAndAdjustStack(code);
                code.ret();
                PerfMapRegister(write_fallbacks[std::make_tuple(ordered, 128, vaddr_idx, value_idx)], code.getCurr(), "a64_write_fallback_128");

                code.align();
                exclusive_write_fallbacks[std::make_tuple(ordered, 128, vaddr_idx, value_idx)] = code.getCurr<void (*)()>();
                ABI_PushCallerSaveRegistersAndAdjustStackExcept(code, HostLoc::RAX);
                if (value_idx != 1) {
                    code.movaps(xmm1, Xbyak::Xmm{value_idx});
                }
                if (code.HasHostFeature(HostFeature::SSE41)) {
                    code.movq(xmm2, rax);
                    code.pinsrq(xmm2, rdx, 1);
                } else {
                    code.movq(xmm2, rax);
                    code.movq(xmm0, rdx);
                    code.punpcklqdq(xmm2, xmm0);
                }
                if (vaddr_idx != code.ABI_PARAM2.getIdx()) {
                    code.mov(code.ABI_PARAM2, Xbyak::Reg64{vaddr_idx});
                }
                code.call(memory_exclusive_write_128);
                ABI_PopCallerSaveRegistersAndAdjustStackExcept(code, HostLoc::RAX);
                code.ret();
                PerfMapRegister(exclusive_write_fallbacks[std::make_tuple(ordered, 128, vaddr_idx, value_idx)], code.getCurr(), "a64_exclusive_write_fallback_128");

                if (value_idx == 4 || value_idx == 15) {
                    continue;
                }

                for (const auto& [bitsize, callback] : read_callbacks) {
                    code.align();
                    read_fallbacks[std::make_tuple(ordered, bitsize, vaddr_idx, value_idx)] = code.getCurr<void (*)()>();
                    ABI_PushCallerSaveRegistersAndAdjustStackExcept(code, HostLocRegIdx(value_idx));
                    // params = { this, vaddr, size }
                    if (vaddr_idx != code.ABI_PARAM2.getIdx()) {
                        code.mov(code.ABI_PARAM2, Xbyak::Reg64{vaddr_idx});
                    }
                    code.mov(code.ABI_PARAM3, bitsize / CHAR_BIT);
                    if (ordered) code.mfence();
                    callback.EmitCall(code);
                    if (value_idx != code.ABI_RETURN.getIdx()) {
                        code.mov(Xbyak::Reg64{value_idx}, code.ABI_RETURN);
                    }
                    ABI_PopCallerSaveRegistersAndAdjustStackExcept(code, HostLocRegIdx(value_idx));
                    code.ZeroExtendFrom(bitsize, Xbyak::Reg64{value_idx});
                    code.ret();
                    PerfMapRegister(read_fallbacks[std::make_tuple(ordered, bitsize, vaddr_idx, value_idx)], code.getCurr(), fmt::format("a64_read_fallback_{}", bitsize));
                }

                for (const auto& [bitsize, callback] : write_callbacks) {
                    code.align();
                    write_fallbacks[std::make_tuple(ordered, bitsize, vaddr_idx, value_idx)] = code.getCurr<void (*)()>();
                    ABI_PushCallerSaveRegistersAndAdjustStack(code);
                    // params = { this, vaddr, value, size }
                    if (vaddr_idx == code.ABI_PARAM3.getIdx() && value_idx == code.ABI_PARAM2.getIdx()) {
                        code.xchg(code.ABI_PARAM2, code.ABI_PARAM3);
                    } else if (vaddr_idx == code.ABI_PARAM3.getIdx()) {
                        code.mov(code.ABI_PARAM2, Xbyak::Reg64{vaddr_idx});
                        if (value_idx != code.ABI_PARAM3.getIdx()) {
                            code.mov(code.ABI_PARAM3, Xbyak::Reg64{value_idx});
                        }
                    } else {
                        if (value_idx != code.ABI_PARAM3.getIdx()) {
                            code.mov(code.ABI_PARAM3, Xbyak::Reg64{value_idx});
                        }
                        if (vaddr_idx != code.ABI_PARAM2.getIdx()) {
                            code.mov(code.ABI_PARAM2, Xbyak::Reg64{vaddr_idx});
                        }
                    }
                    code.ZeroExtendFrom(bitsize, code.ABI_PARAM3);
                    code.mov(code.ABI_PARAM4, bitsize / CHAR_BIT);
                    callback.EmitCall(code);
                    if (ordered) code.mfence();
                    ABI_PopCallerSaveRegistersAndAdjustStack(code);
                    code.ret();
                    PerfMapRegister(write_fallbacks[std::make_tuple(ordered, bitsize, vaddr_idx, value_idx)], code.getCurr(), fmt::format("a64_write_fallback_{}", bitsize));
                }

                for (const auto& [bitsize, callback] : exclusive_write_callbacks) {
                    code.align();
                    exclusive_write_fallbacks[std::make_tuple(ordered, bitsize, vaddr_idx, value_idx)] = code.getCurr<void (*)()>();
                    ABI_PushCallerSaveRegistersAndAdjustStackExcept(code, HostLoc::RAX);
                    if (vaddr_idx == code.ABI_PARAM3.getIdx() && value_idx == code.ABI_PARAM2.getIdx()) {
                        code.xchg(code.ABI_PARAM2, code.ABI_PARAM3);
                    } else if (vaddr_idx == code.ABI_PARAM3.getIdx()) {
                        code.mov(code.ABI_PARAM2, Xbyak::Reg64{vaddr_idx});
                        if (value_idx != code.ABI_PARAM3.getIdx()) {
                            code.mov(code.ABI_PARAM3, Xbyak::Reg64{value_idx});
                        }
                    } else {
                        if (value_idx != code.ABI_PARAM3.getIdx()) {
                            code.mov(code.ABI_PARAM3, Xbyak::Reg64{value_idx});
                        }
                        if (vaddr_idx != code.ABI_PARAM2.getIdx()) {
                            code.mov(code.ABI_PARAM2, Xbyak::Reg64{vaddr_idx});
                        }
                    }
                    code.ZeroExtendFrom(bitsize, code.ABI_PARAM3);
                    code.mov(code.ABI_PARAM4, rax);
                    code.ZeroExtendFrom(bitsize, code.ABI_PARAM4);
                    callback.EmitCall(code);
                    ABI_PopCallerSaveRegistersAndAdjustStackExcept(code, HostLoc::RAX);
                    code.ret();
                    PerfMapRegister(exclusive_write_fallbacks[std::make_tuple(ordered, bitsize, vaddr_idx, value_idx)], code.getCurr(), fmt::format("a64_exclusive_write_fallback_{}", bitsize));
                }
            }
        }
    }
}

#define Axx A64
#include "dynarmic/backend/x64/emit_x64_memory.cpp.inc"
#undef Axx

void A64EmitX64::EmitA64ReadMemory8(A64EmitContext& ctx, IR::Inst* inst) {
    EmitMemoryRead<8, &A64::UserCallbacks::MemoryRead>(ctx, inst);
}

void A64EmitX64::EmitA64ReadMemory16(A64EmitContext& ctx, IR::Inst* inst) {
    EmitMemoryRead<16, &A64::UserCallbacks::MemoryRead>(ctx, inst);
}

void A64EmitX64::EmitA64ReadMemory32(A64EmitContext& ctx, IR::Inst* inst) {
    EmitMemoryRead<32, &A64::UserCallbacks::MemoryRead>(ctx, inst);
}

void A64EmitX64::EmitA64ReadMemory64(A64EmitContext& ctx, IR::Inst* inst) {
    EmitMemoryRead<64, &A64::UserCallbacks::MemoryRead>(ctx, inst);
}

void A64EmitX64::EmitA64ReadMemory128(A64EmitContext& ctx, IR::Inst* inst) {
    EmitMemoryRead<128, &A64::UserCallbacks::MemoryRead128>(ctx, inst);
}

void A64EmitX64::EmitA64WriteMemory8(A64EmitContext& ctx, IR::Inst* inst) {
    EmitMemoryWrite<8, &A64::UserCallbacks::MemoryWrite>(ctx, inst);
}

void A64EmitX64::EmitA64WriteMemory16(A64EmitContext& ctx, IR::Inst* inst) {
    EmitMemoryWrite<16, &A64::UserCallbacks::MemoryWrite>(ctx, inst);
}

void A64EmitX64::EmitA64WriteMemory32(A64EmitContext& ctx, IR::Inst* inst) {
    EmitMemoryWrite<32, &A64::UserCallbacks::MemoryWrite>(ctx, inst);
}

void A64EmitX64::EmitA64WriteMemory64(A64EmitContext& ctx, IR::Inst* inst) {
    EmitMemoryWrite<64, &A64::UserCallbacks::MemoryWrite>(ctx, inst);
}

void A64EmitX64::EmitA64WriteMemory128(A64EmitContext& ctx, IR::Inst* inst) {
    EmitMemoryWrite<128, &A64::UserCallbacks::MemoryWrite>(ctx, inst);
}

void A64EmitX64::EmitA64AtomicCompareExchange32(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2], args[3]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    // Shared compiled blocks must use the executing JIT's callback, monitor and
    // abort target, never the emitter/group owner's captured first-core state.
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u32 expected, u32 desired) -> u32 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        // eden_jit points to private Jit::Impl, not the public A64::Jit wrapper.
        // Halt the actual executing state directly with the same atomic primitive
        // that Impl::HaltExecution uses; never reinterpret its private pointer.
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE casa32_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = callbacks->MemoryAtomicCompareExchange32(
            address, expected, desired, config->global_monitor);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0;
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u32 expected, u32 desired) -> u32 {
        // The callback prepares VM/cache state before acquiring the monitor.
        // Never hold the monitor across a guest-aware validation callback.
        const auto observed = emitter.conf.callbacks->MemoryAtomicCompareExchange32(
            address, expected, desired, emitter.conf.global_monitor);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0; // Not architecturally written: the mandatory check exits first.
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

void A64EmitX64::EmitA64AtomicCompareExchange16(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2], args[3]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    // Shared compiled blocks must use the executing JIT's callback, monitor and
    // abort target, never the emitter/group owner's captured first-core state.
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u32 expected, u32 desired) -> u32 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        // eden_jit points to private Jit::Impl, not the public A64::Jit wrapper.
        // Halt the actual executing state directly with the same atomic primitive
        // that Impl::HaltExecution uses; never reinterpret its private pointer.
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE cas16_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = callbacks->MemoryAtomicCompareExchange16(
            address, static_cast<u16>(expected), static_cast<u16>(desired), config->global_monitor);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0;
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u32 expected, u32 desired) -> u32 {
        // The callback prepares VM/cache state before acquiring the monitor.
        // Never hold the monitor across a guest-aware validation callback.
        const auto observed = emitter.conf.callbacks->MemoryAtomicCompareExchange16(
            address, static_cast<u16>(expected), static_cast<u16>(desired), emitter.conf.global_monitor);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0; // Not architecturally written: the mandatory check exits first.
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

void A64EmitX64::EmitA64AtomicCompareExchange8(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2], args[3]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    // Shared compiled blocks must use the executing JIT's callback, monitor and
    // abort target, never the emitter/group owner's captured first-core state.
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u32 expected, u32 desired) -> u32 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        // eden_jit points to private Jit::Impl, not the public A64::Jit wrapper.
        // Halt the actual executing state directly with the same atomic primitive
        // that Impl::HaltExecution uses; never reinterpret its private pointer.
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE cas8_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = callbacks->MemoryAtomicCompareExchange8(
            address, static_cast<u8>(expected), static_cast<u8>(desired), config->global_monitor);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0;
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u32 expected, u32 desired) -> u32 {
        // The callback prepares VM/cache state before acquiring the monitor.
        // Never hold the monitor across a guest-aware validation callback.
        const auto observed = emitter.conf.callbacks->MemoryAtomicCompareExchange8(
            address, static_cast<u8>(expected), static_cast<u8>(desired), emitter.conf.global_monitor);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0; // Not architecturally written: the mandatory check exits first.
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

void A64EmitX64::EmitA64AtomicCompareExchange64(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2], args[3]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    // Shared compiled blocks must use the executing JIT's callback, monitor and
    // abort target, never the emitter/group owner's captured first-core state.
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u64 expected, u64 desired) -> u64 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        // eden_jit points to private Jit::Impl, not the public A64::Jit wrapper.
        // Halt the actual executing state directly with the same atomic primitive
        // that Impl::HaltExecution uses; never reinterpret its private pointer.
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE casa64_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = callbacks->MemoryAtomicCompareExchange64(
            address, expected, desired, config->global_monitor);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0;
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u64 expected, u64 desired) -> u64 {
        // The callback prepares VM/cache state before acquiring the monitor.
        // Never hold the monitor across a guest-aware validation callback.
        const auto observed = emitter.conf.callbacks->MemoryAtomicCompareExchange64(
            address, expected, desired, emitter.conf.global_monitor);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0; // Not architecturally written: the mandatory check exits first.
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}


// A failed checked CAS only observes memory. The first successful checked CAS
// writes old+operand and returns exactly that old value. No plain load/store or
// exclusive-store status is substituted. Each attempt revalidates VM/coherence
// and takes the existing short monitor-protected real host CAS path.
static std::optional<u32> AtomicFetchAdd32Checked(A64::UserCallbacks& callbacks,
        ExclusiveMonitor* monitor, A64::VAddr address, u32 operand) {
    u32 expected = 0;
    for (;;) {
        const u32 desired = static_cast<u32>(expected + operand);
        const auto observed = callbacks.MemoryAtomicCompareExchange32(address, expected, desired, monitor);
        if (!observed || *observed == expected) return observed;
        expected = *observed;
    }
}

void A64EmitX64::EmitA64AtomicFetchAdd32(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u32 operand) -> u32 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE ldadd32_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = AtomicFetchAdd32Checked(*callbacks, config->global_monitor, address, operand);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0; // Fault placeholder is never written back architecturally.
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u32 operand) -> u32 {
        const auto observed = AtomicFetchAdd32Checked(*emitter.conf.callbacks,
            emitter.conf.global_monitor, address, operand);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0;
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

// Narrow typed checked CAS loop; arithmetic truncates only the memory operand.
static std::optional<u32> AtomicExchange16Checked(A64::UserCallbacks& callbacks,
        ExclusiveMonitor* monitor, A64::VAddr address, u32 operand) {
    u16 expected = 0;
    for (;;) {
        const u16 desired = static_cast<u16>(operand);
        const auto observed = callbacks.MemoryAtomicCompareExchange16(address, expected, desired, monitor);
        if (!observed || *observed == expected) return observed;
        expected = *observed;
    }
}

void A64EmitX64::EmitA64AtomicExchange16(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u32 operand) -> u32 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE swp16orders_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = AtomicExchange16Checked(*callbacks, config->global_monitor, address, operand);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0; // Fault placeholder is never written back architecturally.
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u32 operand) -> u32 {
        const auto observed = AtomicExchange16Checked(*emitter.conf.callbacks,
            emitter.conf.global_monitor, address, operand);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0;
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

// Narrow typed checked CAS loop; arithmetic truncates only the memory operand.
static std::optional<u32> AtomicFetchXor16Checked(A64::UserCallbacks& callbacks,
        ExclusiveMonitor* monitor, A64::VAddr address, u32 operand) {
    u16 expected = 0;
    for (;;) {
        const u16 desired = static_cast<u16>(expected ^ operand);
        const auto observed = callbacks.MemoryAtomicCompareExchange16(address, expected, desired, monitor);
        if (!observed || *observed == expected) return observed;
        expected = *observed;
    }
}

void A64EmitX64::EmitA64AtomicFetchXor16(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u32 operand) -> u32 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE ldeor16orders_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = AtomicFetchXor16Checked(*callbacks, config->global_monitor, address, operand);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0; // Fault placeholder is never written back architecturally.
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u32 operand) -> u32 {
        const auto observed = AtomicFetchXor16Checked(*emitter.conf.callbacks,
            emitter.conf.global_monitor, address, operand);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0;
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

// Narrow typed checked CAS loop; arithmetic truncates only the memory operand.
static std::optional<u32> AtomicFetchOr16Checked(A64::UserCallbacks& callbacks,
        ExclusiveMonitor* monitor, A64::VAddr address, u32 operand) {
    u16 expected = 0;
    for (;;) {
        const u16 desired = static_cast<u16>(expected | operand);
        const auto observed = callbacks.MemoryAtomicCompareExchange16(address, expected, desired, monitor);
        if (!observed || *observed == expected) return observed;
        expected = *observed;
    }
}

void A64EmitX64::EmitA64AtomicFetchOr16(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u32 operand) -> u32 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE ldset16orders_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = AtomicFetchOr16Checked(*callbacks, config->global_monitor, address, operand);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0; // Fault placeholder is never written back architecturally.
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u32 operand) -> u32 {
        const auto observed = AtomicFetchOr16Checked(*emitter.conf.callbacks,
            emitter.conf.global_monitor, address, operand);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0;
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

// Narrow typed checked CAS loop; arithmetic truncates only the memory operand.
static std::optional<u32> AtomicFetchClear16Checked(A64::UserCallbacks& callbacks,
        ExclusiveMonitor* monitor, A64::VAddr address, u32 operand) {
    u16 expected = 0;
    for (;;) {
        const u16 desired = static_cast<u16>(expected & ~operand);
        const auto observed = callbacks.MemoryAtomicCompareExchange16(address, expected, desired, monitor);
        if (!observed || *observed == expected) return observed;
        expected = *observed;
    }
}

void A64EmitX64::EmitA64AtomicFetchClear16(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u32 operand) -> u32 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE ldclr16orders_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = AtomicFetchClear16Checked(*callbacks, config->global_monitor, address, operand);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0; // Fault placeholder is never written back architecturally.
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u32 operand) -> u32 {
        const auto observed = AtomicFetchClear16Checked(*emitter.conf.callbacks,
            emitter.conf.global_monitor, address, operand);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0;
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

// Narrow typed checked CAS loop; arithmetic truncates only the memory operand.
static std::optional<u32> AtomicFetchAdd16Checked(A64::UserCallbacks& callbacks,
        ExclusiveMonitor* monitor, A64::VAddr address, u32 operand) {
    u16 expected = 0;
    for (;;) {
        const u16 desired = static_cast<u16>(expected + operand);
        const auto observed = callbacks.MemoryAtomicCompareExchange16(address, expected, desired, monitor);
        if (!observed || *observed == expected) return observed;
        expected = *observed;
    }
}

void A64EmitX64::EmitA64AtomicFetchAdd16(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u32 operand) -> u32 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE ldadd16orders_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = AtomicFetchAdd16Checked(*callbacks, config->global_monitor, address, operand);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0; // Fault placeholder is never written back architecturally.
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u32 operand) -> u32 {
        const auto observed = AtomicFetchAdd16Checked(*emitter.conf.callbacks,
            emitter.conf.global_monitor, address, operand);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0;
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

// Narrow typed checked CAS loop; arithmetic truncates only the memory operand.
static std::optional<u32> AtomicExchange8Checked(A64::UserCallbacks& callbacks,
        ExclusiveMonitor* monitor, A64::VAddr address, u32 operand) {
    u8 expected = 0;
    for (;;) {
        const u8 desired = static_cast<u8>(operand);
        const auto observed = callbacks.MemoryAtomicCompareExchange8(address, expected, desired, monitor);
        if (!observed || *observed == expected) return observed;
        expected = *observed;
    }
}

void A64EmitX64::EmitA64AtomicExchange8(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u32 operand) -> u32 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE swp8orders_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = AtomicExchange8Checked(*callbacks, config->global_monitor, address, operand);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0; // Fault placeholder is never written back architecturally.
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u32 operand) -> u32 {
        const auto observed = AtomicExchange8Checked(*emitter.conf.callbacks,
            emitter.conf.global_monitor, address, operand);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0;
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

// Narrow typed checked CAS loop; arithmetic truncates only the memory operand.
static std::optional<u32> AtomicFetchXor8Checked(A64::UserCallbacks& callbacks,
        ExclusiveMonitor* monitor, A64::VAddr address, u32 operand) {
    u8 expected = 0;
    for (;;) {
        const u8 desired = static_cast<u8>(expected ^ operand);
        const auto observed = callbacks.MemoryAtomicCompareExchange8(address, expected, desired, monitor);
        if (!observed || *observed == expected) return observed;
        expected = *observed;
    }
}

void A64EmitX64::EmitA64AtomicFetchXor8(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u32 operand) -> u32 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE ldeor8orders_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = AtomicFetchXor8Checked(*callbacks, config->global_monitor, address, operand);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0; // Fault placeholder is never written back architecturally.
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u32 operand) -> u32 {
        const auto observed = AtomicFetchXor8Checked(*emitter.conf.callbacks,
            emitter.conf.global_monitor, address, operand);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0;
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

// Narrow typed checked CAS loop; arithmetic truncates only the memory operand.
static std::optional<u32> AtomicFetchOr8Checked(A64::UserCallbacks& callbacks,
        ExclusiveMonitor* monitor, A64::VAddr address, u32 operand) {
    u8 expected = 0;
    for (;;) {
        const u8 desired = static_cast<u8>(expected | operand);
        const auto observed = callbacks.MemoryAtomicCompareExchange8(address, expected, desired, monitor);
        if (!observed || *observed == expected) return observed;
        expected = *observed;
    }
}

void A64EmitX64::EmitA64AtomicFetchOr8(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u32 operand) -> u32 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE ldset8orders_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = AtomicFetchOr8Checked(*callbacks, config->global_monitor, address, operand);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0; // Fault placeholder is never written back architecturally.
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u32 operand) -> u32 {
        const auto observed = AtomicFetchOr8Checked(*emitter.conf.callbacks,
            emitter.conf.global_monitor, address, operand);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0;
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

// Narrow typed checked CAS loop; arithmetic truncates only the memory operand.
static std::optional<u32> AtomicFetchClear8Checked(A64::UserCallbacks& callbacks,
        ExclusiveMonitor* monitor, A64::VAddr address, u32 operand) {
    u8 expected = 0;
    for (;;) {
        const u8 desired = static_cast<u8>(expected & ~operand);
        const auto observed = callbacks.MemoryAtomicCompareExchange8(address, expected, desired, monitor);
        if (!observed || *observed == expected) return observed;
        expected = *observed;
    }
}

void A64EmitX64::EmitA64AtomicFetchClear8(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u32 operand) -> u32 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE ldclr8orders_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = AtomicFetchClear8Checked(*callbacks, config->global_monitor, address, operand);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0; // Fault placeholder is never written back architecturally.
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u32 operand) -> u32 {
        const auto observed = AtomicFetchClear8Checked(*emitter.conf.callbacks,
            emitter.conf.global_monitor, address, operand);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0;
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

// Narrow typed checked CAS loop; arithmetic truncates only the memory operand.
static std::optional<u32> AtomicFetchAdd8Checked(A64::UserCallbacks& callbacks,
        ExclusiveMonitor* monitor, A64::VAddr address, u32 operand) {
    u8 expected = 0;
    for (;;) {
        const u8 desired = static_cast<u8>(expected + operand);
        const auto observed = callbacks.MemoryAtomicCompareExchange8(address, expected, desired, monitor);
        if (!observed || *observed == expected) return observed;
        expected = *observed;
    }
}

void A64EmitX64::EmitA64AtomicFetchAdd8(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u32 operand) -> u32 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE ldadd8orders_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = AtomicFetchAdd8Checked(*callbacks, config->global_monitor, address, operand);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0; // Fault placeholder is never written back architecturally.
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u32 operand) -> u32 {
        const auto observed = AtomicFetchAdd8Checked(*emitter.conf.callbacks,
            emitter.conf.global_monitor, address, operand);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0;
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

// Checked real CAS loop, old-value result, conservative full barriers.
static std::optional<u32> AtomicExchange32Checked(A64::UserCallbacks& callbacks,
        ExclusiveMonitor* monitor, A64::VAddr address, u32 operand) {
    u32 expected = 0;
    for (;;) {
        const u32 desired = static_cast<u32>(operand);
        const auto observed = callbacks.MemoryAtomicCompareExchange32(address, expected, desired, monitor);
        if (!observed || *observed == expected) return observed;
        expected = *observed;
    }
}

void A64EmitX64::EmitA64AtomicExchange32(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u32 operand) -> u32 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE swp32orders_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = AtomicExchange32Checked(*callbacks, config->global_monitor, address, operand);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0; // Fault placeholder is never written back architecturally.
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u32 operand) -> u32 {
        const auto observed = AtomicExchange32Checked(*emitter.conf.callbacks,
            emitter.conf.global_monitor, address, operand);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0;
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

// Checked real CAS loop, old-value result, conservative full barriers.
static std::optional<u32> AtomicFetchXor32Checked(A64::UserCallbacks& callbacks,
        ExclusiveMonitor* monitor, A64::VAddr address, u32 operand) {
    u32 expected = 0;
    for (;;) {
        const u32 desired = static_cast<u32>(expected ^ operand);
        const auto observed = callbacks.MemoryAtomicCompareExchange32(address, expected, desired, monitor);
        if (!observed || *observed == expected) return observed;
        expected = *observed;
    }
}

void A64EmitX64::EmitA64AtomicFetchXor32(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u32 operand) -> u32 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE ldeor32orders_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = AtomicFetchXor32Checked(*callbacks, config->global_monitor, address, operand);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0; // Fault placeholder is never written back architecturally.
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u32 operand) -> u32 {
        const auto observed = AtomicFetchXor32Checked(*emitter.conf.callbacks,
            emitter.conf.global_monitor, address, operand);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0;
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

// Full-width fetch-add, linearized solely by the checked real CAS64 callback.
static std::optional<u64> AtomicFetchAdd64Checked(A64::UserCallbacks& callbacks,
        ExclusiveMonitor* monitor, A64::VAddr address, u64 operand) {
    u64 expected = 0;
    for (;;) {
        const u64 desired = static_cast<u64>(expected + operand);
        const auto observed = callbacks.MemoryAtomicCompareExchange64(address, expected, desired, monitor);
        if (!observed || *observed == expected) return observed;
        expected = *observed;
    }
}

void A64EmitX64::EmitA64AtomicFetchAdd64(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u64 operand) -> u64 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE ldadd64_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = AtomicFetchAdd64Checked(*callbacks, config->global_monitor, address, operand);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0; // Fault placeholder is never written back architecturally.
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u64 operand) -> u64 {
        const auto observed = AtomicFetchAdd64Checked(*emitter.conf.callbacks,
            emitter.conf.global_monitor, address, operand);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0;
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

// Checked real CAS loop, old-value result, conservative full barriers.
static std::optional<u64> AtomicExchange64Checked(A64::UserCallbacks& callbacks,
        ExclusiveMonitor* monitor, A64::VAddr address, u64 operand) {
    u64 expected = 0;
    for (;;) {
        const u64 desired = static_cast<u64>(operand);
        const auto observed = callbacks.MemoryAtomicCompareExchange64(address, expected, desired, monitor);
        if (!observed || *observed == expected) return observed;
        expected = *observed;
    }
}

void A64EmitX64::EmitA64AtomicExchange64(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u64 operand) -> u64 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE swp64orders_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = AtomicExchange64Checked(*callbacks, config->global_monitor, address, operand);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0; // Fault placeholder is never written back architecturally.
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u64 operand) -> u64 {
        const auto observed = AtomicExchange64Checked(*emitter.conf.callbacks,
            emitter.conf.global_monitor, address, operand);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0;
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

// Checked real CAS loop, old-value result, conservative full barriers.
static std::optional<u64> AtomicFetchXor64Checked(A64::UserCallbacks& callbacks,
        ExclusiveMonitor* monitor, A64::VAddr address, u64 operand) {
    u64 expected = 0;
    for (;;) {
        const u64 desired = static_cast<u64>(expected ^ operand);
        const auto observed = callbacks.MemoryAtomicCompareExchange64(address, expected, desired, monitor);
        if (!observed || *observed == expected) return observed;
        expected = *observed;
    }
}

void A64EmitX64::EmitA64AtomicFetchXor64(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u64 operand) -> u64 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE ldeor64orders_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = AtomicFetchXor64Checked(*callbacks, config->global_monitor, address, operand);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0; // Fault placeholder is never written back architecturally.
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u64 operand) -> u64 {
        const auto observed = AtomicFetchXor64Checked(*emitter.conf.callbacks,
            emitter.conf.global_monitor, address, operand);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0;
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

// Checked real CAS loop, old-value result, conservative full barriers.
static std::optional<u64> AtomicFetchClear64Checked(A64::UserCallbacks& callbacks,
        ExclusiveMonitor* monitor, A64::VAddr address, u64 operand) {
    u64 expected = 0;
    for (;;) {
        const u64 desired = static_cast<u64>(expected & ~operand);
        const auto observed = callbacks.MemoryAtomicCompareExchange64(address, expected, desired, monitor);
        if (!observed || *observed == expected) return observed;
        expected = *observed;
    }
}

void A64EmitX64::EmitA64AtomicFetchClear64(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u64 operand) -> u64 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE ldclr64orders_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = AtomicFetchClear64Checked(*callbacks, config->global_monitor, address, operand);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0; // Fault placeholder is never written back architecturally.
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u64 operand) -> u64 {
        const auto observed = AtomicFetchClear64Checked(*emitter.conf.callbacks,
            emitter.conf.global_monitor, address, operand);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0;
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

// Checked real CAS loop, old-value result, conservative full barriers.
static std::optional<u64> AtomicFetchOr64Checked(A64::UserCallbacks& callbacks,
        ExclusiveMonitor* monitor, A64::VAddr address, u64 operand) {
    u64 expected = 0;
    for (;;) {
        const u64 desired = static_cast<u64>(expected | operand);
        const auto observed = callbacks.MemoryAtomicCompareExchange64(address, expected, desired, monitor);
        if (!observed || *observed == expected) return observed;
        expected = *observed;
    }
}

void A64EmitX64::EmitA64AtomicFetchOr64(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u64 operand) -> u64 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE ldset64orders_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = AtomicFetchOr64Checked(*callbacks, config->global_monitor, address, operand);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0; // Fault placeholder is never written back architecturally.
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u64 operand) -> u64 {
        const auto observed = AtomicFetchOr64Checked(*emitter.conf.callbacks,
            emitter.conf.global_monitor, address, operand);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0;
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

// Same checked VM/monitor/CAS path; OR replaces arithmetic addition.
static std::optional<u32> AtomicFetchOr32Checked(A64::UserCallbacks& callbacks,
        ExclusiveMonitor* monitor, A64::VAddr address, u32 operand) {
    u32 expected = 0;
    for (;;) {
        const u32 desired = static_cast<u32>(expected | operand);
        const auto observed = callbacks.MemoryAtomicCompareExchange32(address, expected, desired, monitor);
        if (!observed || *observed == expected) return observed;
        expected = *observed;
    }
}

void A64EmitX64::EmitA64AtomicFetchOr32(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u32 operand) -> u32 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE ldset32_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = AtomicFetchOr32Checked(*callbacks, config->global_monitor, address, operand);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0; // Fault placeholder is never written back architecturally.
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u32 operand) -> u32 {
        const auto observed = AtomicFetchOr32Checked(*emitter.conf.callbacks,
            emitter.conf.global_monitor, address, operand);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0;
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

// Same checked VM/monitor/CAS path; AND-NOT replaces arithmetic addition.
static std::optional<u32> AtomicFetchClear32Checked(A64::UserCallbacks& callbacks,
        ExclusiveMonitor* monitor, A64::VAddr address, u32 operand) {
    u32 expected = 0;
    for (;;) {
        const u32 desired = static_cast<u32>(expected & ~operand);
        const auto observed = callbacks.MemoryAtomicCompareExchange32(address, expected, desired, monitor);
        if (!observed || *observed == expected) return observed;
        expected = *observed;
    }
}

void A64EmitX64::EmitA64AtomicFetchClear32(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
#ifdef EDEN_SHARED_JIT_LAYOUT
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallLambda([](A64JitState& state, A64::VAddr address, u32 operand) -> u32 {
        const auto* config = static_cast<const A64::UserConfig*>(state.eden_conf);
        auto* callbacks = static_cast<A64::UserCallbacks*>(state.eden_callbacks);
        if (!config || !callbacks || !state.eden_jit) {
            std::fputs("CPU_TRACE ldclr32_runtime_context_missing action=abort_no_fake_success\n", stderr);
            std::abort();
        }
        const auto observed = AtomicFetchClear32Checked(*callbacks, config->global_monitor, address, operand);
        if (!observed) {
            Atomic::Or(&state.halt_reason, static_cast<u32>(HaltReason::MemoryAbort));
            return 0; // Fault placeholder is never written back architecturally.
        }
        return *observed;
    });
#else
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(this));
    code.CallLambda([](A64EmitX64& emitter, A64::VAddr address, u32 operand) -> u32 {
        const auto observed = AtomicFetchClear32Checked(*emitter.conf.callbacks,
            emitter.conf.global_monitor, address, operand);
        if (!observed) {
            emitter.jit_interface->HaltExecution(HaltReason::MemoryAbort);
            return 0;
        }
        return *observed;
    });
#endif
    EmitCheckMemoryAbort(ctx, inst);
}

// Same checked VM/monitor/CAS path; OR replaces arithmetic addition.
void A64EmitX64::EmitA64ClearExclusive(A64EmitContext&, IR::Inst*) {
    code.mov(code.byte[code.ABI_JIT_PTR + offsetof(A64JitState, exclusive_state)], u8(0));
}

void A64EmitX64::EmitA64ExclusiveReadMemory8(A64EmitContext& ctx, IR::Inst* inst) {
    if (conf.fastmem_exclusive_access) {
        EmitExclusiveReadMemoryInline<8, &A64::UserCallbacks::MemoryRead>(ctx, inst);
    } else {
        EmitExclusiveReadMemory<8, &A64::UserCallbacks::MemoryRead>(ctx, inst);
    }
}

void A64EmitX64::EmitA64ExclusiveReadMemory16(A64EmitContext& ctx, IR::Inst* inst) {
    if (conf.fastmem_exclusive_access) {
        EmitExclusiveReadMemoryInline<16, &A64::UserCallbacks::MemoryRead>(ctx, inst);
    } else {
        EmitExclusiveReadMemory<16, &A64::UserCallbacks::MemoryRead>(ctx, inst);
    }
}

void A64EmitX64::EmitA64ExclusiveReadMemory32(A64EmitContext& ctx, IR::Inst* inst) {
    if (conf.fastmem_exclusive_access) {
        EmitExclusiveReadMemoryInline<32, &A64::UserCallbacks::MemoryRead>(ctx, inst);
    } else {
        EmitExclusiveReadMemory<32, &A64::UserCallbacks::MemoryRead>(ctx, inst);
    }
}

void A64EmitX64::EmitA64ExclusiveReadMemory64(A64EmitContext& ctx, IR::Inst* inst) {
    if (conf.fastmem_exclusive_access) {
        EmitExclusiveReadMemoryInline<64, &A64::UserCallbacks::MemoryRead>(ctx, inst);
    } else {
        EmitExclusiveReadMemory<64, &A64::UserCallbacks::MemoryRead>(ctx, inst);
    }
}

void A64EmitX64::EmitA64ExclusiveReadMemory128(A64EmitContext& ctx, IR::Inst* inst) {
    if (conf.fastmem_exclusive_access) {
        EmitExclusiveReadMemoryInline<128, &A64::UserCallbacks::MemoryRead128>(ctx, inst);
    } else {
        EmitExclusiveReadMemory<128, &A64::UserCallbacks::MemoryRead128>(ctx, inst);
    }
}

void A64EmitX64::EmitA64ExclusiveWriteMemory8(A64EmitContext& ctx, IR::Inst* inst) {
    if (conf.fastmem_exclusive_access) {
        EmitExclusiveWriteMemoryInline<8, &A64::UserCallbacks::MemoryWriteExclusive8>(ctx, inst);
    } else {
        EmitExclusiveWriteMemory<8, &A64::UserCallbacks::MemoryWriteExclusive8>(ctx, inst);
    }
}

void A64EmitX64::EmitA64ExclusiveWriteMemory16(A64EmitContext& ctx, IR::Inst* inst) {
    if (conf.fastmem_exclusive_access) {
        EmitExclusiveWriteMemoryInline<16, &A64::UserCallbacks::MemoryWriteExclusive16>(ctx, inst);
    } else {
        EmitExclusiveWriteMemory<16, &A64::UserCallbacks::MemoryWriteExclusive16>(ctx, inst);
    }
}

void A64EmitX64::EmitA64ExclusiveWriteMemory32(A64EmitContext& ctx, IR::Inst* inst) {
    if (conf.fastmem_exclusive_access) {
        EmitExclusiveWriteMemoryInline<32, &A64::UserCallbacks::MemoryWriteExclusive32>(ctx, inst);
    } else {
        EmitExclusiveWriteMemory<32, &A64::UserCallbacks::MemoryWriteExclusive32>(ctx, inst);
    }
}

void A64EmitX64::EmitA64ExclusiveWriteMemory64(A64EmitContext& ctx, IR::Inst* inst) {
    if (conf.fastmem_exclusive_access) {
        EmitExclusiveWriteMemoryInline<64, &A64::UserCallbacks::MemoryWriteExclusive64>(ctx, inst);
    } else {
        EmitExclusiveWriteMemory<64, &A64::UserCallbacks::MemoryWriteExclusive64>(ctx, inst);
    }
}

void A64EmitX64::EmitA64ExclusiveWriteMemory128(A64EmitContext& ctx, IR::Inst* inst) {
    if (conf.fastmem_exclusive_access) {
        EmitExclusiveWriteMemoryInline<128, &A64::UserCallbacks::MemoryWriteExclusive128>(ctx, inst);
    } else {
        EmitExclusiveWriteMemory<128, &A64::UserCallbacks::MemoryWriteExclusive128>(ctx, inst);
    }
}

void A64EmitX64::EmitCheckMemoryAbort(A64EmitContext&, IR::Inst* inst, Xbyak::Label* end) {
    if (!conf.check_halt_on_memory_access && inst->GetOpcode() != IR::Opcode::A64AtomicCompareExchange32 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicCompareExchange16 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicCompareExchange8 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicFetchAdd32 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicExchange16 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicFetchXor16 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicFetchOr16 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicFetchClear16 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicFetchAdd16 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicExchange8 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicFetchXor8 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicFetchOr8 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicFetchClear8 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicFetchAdd8 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicExchange32 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicFetchXor32 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicExchange64 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicFetchXor64 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicFetchClear64 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicFetchOr64 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicFetchAdd64 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicFetchClear32 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicFetchOr32 &&
        inst->GetOpcode() != IR::Opcode::A64AtomicCompareExchange64) {
        return;
    }

    Xbyak::Label skip;

    const A64::LocationDescriptor current_location{IR::LocationDescriptor{inst->GetArg(0).GetU64()}};

    code.test(dword[code.ABI_JIT_PTR + offsetof(A64JitState, halt_reason)], static_cast<u32>(HaltReason::MemoryAbort));
    if (end) {
        code.jz(*end, code.T_NEAR);
    } else {
        code.jz(skip, code.T_NEAR);
    }
    code.mov(rax, current_location.PC());
    code.mov(qword[code.ABI_JIT_PTR + offsetof(A64JitState, pc)], rax);
    code.ForceReturnFromRunCode();
    code.L(skip);
}

}  // namespace Dynarmic::Backend::X64
