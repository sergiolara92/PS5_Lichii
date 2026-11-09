# ProsperoEden: up to two A64 guest registers kept in host registers across blocks (dev-settings
# jit_pin=<n>[,<n>], off by default while measured). Upstream writes every guest register a block
# sets back to the JIT state at the block's end and loads it again in the next block; in a CPU-bound
# game's short blocks a quarter of the sampled time sat on those loads and stores. A pinned register
# lives in R12 (first) or R13 (second) for the whole RunCode/StepCode: both are callee-saved, so C++
# callees keep them, and they leave the allocator's set. R13 is the fastmem window's base, so a JIT
# with fastmem keeps one pin (jit_impl.inc, EdenPinInfo); RBX is never a pin, as the 128-bit ordered
# and exclusive stores take it for cmpxchg16b. The JIT state copy is written back before
# every call out of JIT code (BlockOfCode::CallFunction/CallLambda, CallHostFunction) and on the way
# out of RunCode, and loaded on the way in, so whatever C++ reads (SVC arguments, exception
# contexts, backtraces) is current. The A64 terminal handlers, which used RBX and R12 as temporaries,
# use RDX and RSI then, with RSI kept in a spill slot across the lookup call. A32 JITs never pin
# (JitStateInfo only fills the pin list for a 64-bit program counter).

function(eden_reg_pin_require source anchor what)
    string(FIND "${source}" "${anchor}" at)
    if(at LESS 0)
        message(FATAL_ERROR "Pinned JIT register-pinning anchor changed (${what})")
    endif()
endfunction()

# jitstate_info.h: the pin list, filled for 64-bit JIT states only.
macro(eden_reg_pin_jitstate_info)
    set(eden_rp_info_rel "dynarmic/backend/x64/jitstate_info.h")
    file(READ "${PROJECT_SOURCE_DIR}/src/dynarmic/src/${eden_rp_info_rel}" eden_rp_info)
    set(eden_rp_ctor_end "            , offsetof_halt_reason(offsetof(JitStateType, halt_reason)) {}\n")
    set(eden_rp_last "    const size_t offsetof_halt_reason;\n")
    eden_reg_pin_require("${eden_rp_info}" "${eden_rp_ctor_end}" "jitstate_info.h constructor")
    eden_reg_pin_require("${eden_rp_info}" "${eden_rp_last}" "jitstate_info.h members")
    string(REPLACE "${eden_rp_ctor_end}" [==[            , offsetof_halt_reason(offsetof(JitStateType, halt_reason)) {
        // ProsperoEden (headless/dynarmic/reg_pin.cmake): only 64-bit guests pin registers.
        if constexpr (sizeof(typename JitStateType::ProgramCounterType) == 8) {
            eden_reg_offset = offsetof(JitStateType, reg);
            eden_pin_count = eden_jit_pin_count > 2 ? 2 : eden_jit_pin_count;
            for (unsigned i = 0; i < eden_pin_count; ++i)
                eden_pin_guest[i] = eden_jit_pins[i];
        }
    }
]==] eden_rp_info "${eden_rp_info}")
    string(REPLACE "${eden_rp_last}" "${eden_rp_last}    size_t eden_reg_offset = 0;\n    unsigned eden_pin_count = 0;\n    unsigned char eden_pin_guest[2] = {};\n" eden_rp_info "${eden_rp_info}")
    write_derived("${PORT_BUILD_DIR}/include/${eden_rp_info_rel}"
        "extern \"C\" unsigned eden_jit_pin_count;  // headless/dynarmic/reg_pin.cmake, set before any JIT exists\nextern \"C\" unsigned char eden_jit_pins[2];\n${eden_rp_info}")
endmacro()

# block_of_code.h (code_header in CMakeLists.txt): pin helpers; every C++ call writes the pins back.
macro(eden_reg_pin_code_header)
    set(eden_rp_call [==[    void CallFunction(F fn) {
        ::Common::X64::CallFarFunction(*this, fn);]==])
    set(eden_rp_lambda [==[    void CallLambda(Lambda l) {
        ::Common::X64::CallFarFunction(*this, Common::FptrCast(l));]==])
    set(eden_rp_getinfo "    JitStateInfo GetJitStateInfo() const { return jsi; }\n")
    foreach(anchor IN ITEMS eden_rp_call eden_rp_lambda eden_rp_getinfo)
        eden_reg_pin_require("${code_header}" "${${anchor}}" "block_of_code.h ${anchor}")
    endforeach()
    string(REPLACE "${eden_rp_call}" [==[    void CallFunction(F fn) {
        EdenStorePins();
        ::Common::X64::CallFarFunction(*this, fn);]==] code_header "${code_header}")
    string(REPLACE "${eden_rp_lambda}" [==[    void CallLambda(Lambda l) {
        EdenStorePins();
        ::Common::X64::CallFarFunction(*this, Common::FptrCast(l));]==] code_header "${code_header}")
    string(REPLACE "${eden_rp_getinfo}" [==[    JitStateInfo GetJitStateInfo() const { return jsi; }

    /// ProsperoEden (headless/dynarmic/reg_pin.cmake): the host register of pin i.
    static Xbyak::Reg64 EdenPinHost(unsigned i) { return i == 0 ? Xbyak::util::r12 : Xbyak::util::r13; }
    /// The pin holding A64 guest register `guest`, or -1.
    int EdenPinOf(size_t guest) const {
        for (unsigned i = 0; i < jsi.eden_pin_count; ++i)
            if (jsi.eden_pin_guest[i] == guest) return int(i);
        return -1;
    }
    unsigned EdenPinCount() const { return jsi.eden_pin_count; }
    /// Code emitter: pinned registers to the JIT state's copy (before C++ may read it).
    void EdenStorePins() {
        for (unsigned i = 0; i < jsi.eden_pin_count; ++i)
            mov(qword[ABI_JIT_PTR + jsi.eden_reg_offset + 8 * jsi.eden_pin_guest[i]], EdenPinHost(i));
    }
    /// Code emitter: pinned registers from the JIT state (on entry).
    void EdenLoadPins() {
        for (unsigned i = 0; i < jsi.eden_pin_count; ++i)
            mov(EdenPinHost(i), qword[ABI_JIT_PTR + jsi.eden_reg_offset + 8 * jsi.eden_pin_guest[i]]);
    }
]==] code_header "${code_header}")
endmacro()

# block_of_code.cpp (code_source): load on entry, write back on the way out.
macro(eden_reg_pin_code_source)
    set(eden_rp_run_entry "    mov(ABI_JIT_PTR, ABI_PARAM1);\n    mov(rbx, ABI_PARAM2); // save temporarily in non-volatile register\n")
    set(eden_rp_run_jump "    SwitchMxcsrOnEntry();\n    jmp(rbx);\n")
    set(eden_rp_step_entry "    mov(ABI_JIT_PTR, ABI_PARAM1);\n\n    if (cb.enable_cycle_counting) {\n        mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)], 1);\n")
    set(eden_rp_exit "    L(return_to_caller_mxcsr_already_exited);\n")
    foreach(anchor IN ITEMS eden_rp_run_entry eden_rp_run_jump eden_rp_step_entry eden_rp_exit)
        eden_reg_pin_require("${code_source}" "${${anchor}}" "block_of_code.cpp ${anchor}")
    endforeach()
    string(REPLACE "${eden_rp_run_entry}" "${eden_rp_run_entry}    EdenLoadPins();  // ProsperoEden (headless/dynarmic/reg_pin.cmake)\n" code_source "${code_source}")
    string(REPLACE "${eden_rp_step_entry}" "    mov(ABI_JIT_PTR, ABI_PARAM1);\n    EdenLoadPins();  // ProsperoEden (headless/dynarmic/reg_pin.cmake)\n\n    if (cb.enable_cycle_counting) {\n        mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)], 1);\n" code_source "${code_source}")
    string(REPLACE "${eden_rp_exit}" "${eden_rp_exit}    EdenStorePins();  // ProsperoEden (headless/dynarmic/reg_pin.cmake)\n" code_source "${code_source}")
endmacro()

# emit_x64.cpp (emitter in CMakeLists.txt): the IR's direct host calls write the pins back too.
macro(eden_reg_pin_emit_x64)
    set(eden_rp_host_call "    auto target = args[0].GetImmediateU64();\n    if (IsWithin2G(uintptr_t(code.getCurr()), target)) {\n")
    eden_reg_pin_require("${emitter}" "${eden_rp_host_call}" "emit_x64.cpp CallHostFunction")
    string(REPLACE "${eden_rp_host_call}" "    auto target = args[0].GetImmediateU64();\n    code.EdenStorePins();  // ProsperoEden (headless/dynarmic/reg_pin.cmake)\n    if (IsWithin2G(uintptr_t(code.getCurr()), target)) {\n" emitter "${emitter}")
endmacro()

# a64_emit_x64.cpp (emitter, 64-bit only): allocator set, guest register access, terminal handlers.
macro(eden_reg_pin_a64_emitter)
    set(eden_rp_gprs "        if (conf.page_table)\n            gprs.reset(size_t(HostLoc::R14));\n        return gprs;\n")
    set(eden_rp_getw "    const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr(code).cvt32();\n\n    code.mov(result, dword[code.ABI_JIT_PTR + offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg)]);\n")
    set(eden_rp_getx "    const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr(code);\n\n    code.mov(result, qword[code.ABI_JIT_PTR + offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg)]);\n")
    set(eden_rp_setw "void A64EmitX64::EmitA64SetW(A64EmitContext& ctx, IR::Inst* inst) {\n    auto args = ctx.reg_alloc.GetArgumentInfo(inst);\n    const A64::Reg reg = inst->GetArg(0).GetA64RegRef();\n")
    set(eden_rp_setx "void A64EmitX64::EmitA64SetX(A64EmitContext& ctx, IR::Inst* inst) {\n    auto args = ctx.reg_alloc.GetArgumentInfo(inst);\n    const A64::Reg reg = inst->GetArg(0).GetA64RegRef();\n")
    set(eden_rp_handlers_begin "void A64EmitX64::GenTerminalHandlers() {\n")
    set(eden_rp_handlers_end "void A64EmitX64::EmitPushRSB(")
    foreach(anchor IN ITEMS eden_rp_gprs eden_rp_getw eden_rp_getx eden_rp_setw eden_rp_setx eden_rp_handlers_begin eden_rp_handlers_end)
        eden_reg_pin_require("${emitter}" "${${anchor}}" "a64_emit_x64.cpp ${anchor}")
    endforeach()
    string(REPLACE "${eden_rp_gprs}" "        if (conf.page_table)\n            gprs.reset(size_t(HostLoc::R14));\n        // ProsperoEden (headless/dynarmic/reg_pin.cmake): pinned guest registers.\n        if (code.EdenPinCount() > 0)\n            gprs.reset(size_t(HostLoc::R12));\n        if (code.EdenPinCount() > 1)\n            gprs.reset(size_t(HostLoc::R13));\n        return gprs;\n" emitter "${emitter}")
    string(REPLACE "${eden_rp_getw}" [==[    const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr(code).cvt32();

    if (const int pin = code.EdenPinOf(size_t(reg)); pin >= 0) {
        code.mov(result, BlockOfCode::EdenPinHost(unsigned(pin)).cvt32());
        ctx.reg_alloc.DefineValue(code, inst, result);
        return;
    }
    code.mov(result, dword[code.ABI_JIT_PTR + offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg)]);
]==] emitter "${emitter}")
    string(REPLACE "${eden_rp_getx}" [==[    const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr(code);

    if (const int pin = code.EdenPinOf(size_t(reg)); pin >= 0) {
        code.mov(result, BlockOfCode::EdenPinHost(unsigned(pin)));
        ctx.reg_alloc.DefineValue(code, inst, result);
        return;
    }
    code.mov(result, qword[code.ABI_JIT_PTR + offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg)]);
]==] emitter "${emitter}")
    string(REPLACE "${eden_rp_setw}" [==[void A64EmitX64::EmitA64SetW(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const A64::Reg reg = inst->GetArg(0).GetA64RegRef();
    if (const int pin = code.EdenPinOf(size_t(reg)); pin >= 0) {
        // A W write clears the upper half: a 32-bit mov does.
        const Xbyak::Reg32 host = BlockOfCode::EdenPinHost(unsigned(pin)).cvt32();
        if (args[1].IsImmediate())
            code.mov(host, u32(args[1].GetImmediateU64()));
        else
            code.mov(host, ctx.reg_alloc.UseGpr(code, args[1]).cvt32());
        return;
    }
]==] emitter "${emitter}")
    string(REPLACE "${eden_rp_setx}" [==[void A64EmitX64::EmitA64SetX(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const A64::Reg reg = inst->GetArg(0).GetA64RegRef();
    if (const int pin = code.EdenPinOf(size_t(reg)); pin >= 0) {
        const Xbyak::Reg64 host = BlockOfCode::EdenPinHost(unsigned(pin));
        if (args[1].IsImmediate())
            code.mov(host, args[1].GetImmediateU64());
        else if (args[1].IsInXmm(ctx.reg_alloc))
            code.movq(host, ctx.reg_alloc.UseXmm(code, args[1]));
        else
            code.mov(host, ctx.reg_alloc.UseGpr(code, args[1]));
        return;
    }
]==] emitter "${emitter}")
    # Terminal handlers: RBX/R12 temporaries become RDX/RSI when pinning.
    string(FIND "${emitter}" "${eden_rp_handlers_begin}" eden_rp_hb)
    string(FIND "${emitter}" "${eden_rp_handlers_end}" eden_rp_he)
    string(SUBSTRING "${emitter}" 0 ${eden_rp_hb} eden_rp_before)
    math(EXPR eden_rp_len "${eden_rp_he} - ${eden_rp_hb}")
    string(SUBSTRING "${emitter}" ${eden_rp_hb} ${eden_rp_len} eden_rp_body)
    string(SUBSTRING "${emitter}" ${eden_rp_he} -1 eden_rp_after)
    foreach(token IN ITEMS "const auto calculate_location_descriptor = [this] {" "code.mov(ebx," "code.and_(ebx," "code.shl(rbx," "code.or_(rbx," "code.cmp(rbx," "code.mov(r12, rbx)" "code.crc32(r12," "code.and_(r12d," "code.lea(r12, ptr[r8 + r12])" "qword[r12 +" "ptr[r12 +" "], rbx);" "code.LookupBlock();")
        eden_reg_pin_require("${eden_rp_body}" "${token}" "GenTerminalHandlers ${token}")
    endforeach()
    string(REPLACE "const auto calculate_location_descriptor = [this] {" "const auto calculate_location_descriptor = [this, eden_loc] {" eden_rp_body "${eden_rp_body}")
    string(REPLACE "code.mov(ebx," "code.mov(eden_loc.cvt32()," eden_rp_body "${eden_rp_body}")
    string(REPLACE "code.and_(ebx," "code.and_(eden_loc.cvt32()," eden_rp_body "${eden_rp_body}")
    string(REPLACE "code.shl(rbx," "code.shl(eden_loc," eden_rp_body "${eden_rp_body}")
    string(REPLACE "code.or_(rbx," "code.or_(eden_loc," eden_rp_body "${eden_rp_body}")
    string(REPLACE "code.cmp(rbx," "code.cmp(eden_loc," eden_rp_body "${eden_rp_body}")
    string(REPLACE "code.mov(r12, rbx)" "code.mov(eden_ent, eden_loc)" eden_rp_body "${eden_rp_body}")
    string(REPLACE "code.crc32(r12," "code.crc32(eden_ent," eden_rp_body "${eden_rp_body}")
    string(REPLACE "code.and_(r12d," "code.and_(eden_ent.cvt32()," eden_rp_body "${eden_rp_body}")
    string(REPLACE "code.lea(r12, ptr[r8 + r12])" "code.lea(eden_ent, ptr[r8 + eden_ent])" eden_rp_body "${eden_rp_body}")
    string(REPLACE "qword[r12 +" "qword[eden_ent +" eden_rp_body "${eden_rp_body}")
    string(REPLACE "ptr[r12 +" "ptr[eden_ent +" eden_rp_body "${eden_rp_body}")
    string(REPLACE "], rbx);" "], eden_loc);" eden_rp_body "${eden_rp_body}")
    string(REPLACE "code.LookupBlock();" [==[if (eden_pinning) code.mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, spill)], eden_ent);
        code.LookupBlock();
        if (eden_pinning) code.mov(eden_ent, qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, spill)]);]==] eden_rp_body "${eden_rp_body}")
    string(REPLACE "${eden_rp_handlers_begin}" [==[void A64EmitX64::GenTerminalHandlers() {
    // ProsperoEden (headless/dynarmic/reg_pin.cmake): RBX and R12 may hold pinned guest registers.
    const bool eden_pinning = code.EdenPinCount() > 0;
    const Xbyak::Reg64 eden_loc = eden_pinning ? rdx : rbx;
    const Xbyak::Reg64 eden_ent = eden_pinning ? rsi : r12;
]==] eden_rp_body "${eden_rp_body}")
    set(emitter "${eden_rp_before}${eden_rp_body}${eden_rp_after}")
endmacro()
