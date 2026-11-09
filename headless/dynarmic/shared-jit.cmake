# ProsperoEden: one compiled-code namespace per guest process for the A64 JITs
# (headless/dynarmic/jit_group.h). Each macro edits a source already read into the caller's
# variable, just before the caller writes the derived file.

set(EDEN_SHARED_JIT_DIR "${CMAKE_CURRENT_LIST_DIR}")
file(GLOB eden_shared_jit_inputs CONFIGURE_DEPENDS "${EDEN_SHARED_JIT_DIR}/*.inc" "${EDEN_SHARED_JIT_DIR}/*.h"
     "${EDEN_SHARED_JIT_DIR}/*.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${eden_shared_jit_inputs})

function(eden_shared_jit_require source anchor what)
    string(FIND "${source}" "${anchor}" at)
    if(at LESS 0)
        message(FATAL_ERROR "Pinned ${what} changed for the shared JIT")
    endif()
endfunction()

# a64_emit_x64.h: the group and the guest range of the last Emit.
macro(eden_shared_jit_emitter_header)
    eden_shared_jit_require("${emitter_header}" "    A64::Jit* jit_interface = nullptr;" "A64 emitter members")
    eden_shared_jit_require("${emitter_header}" "#include \"dynarmic/backend/x64/emit_x64.h\"" "A64 emitter includes")
    string(REPLACE "#include \"dynarmic/backend/x64/emit_x64.h\""
        "#include \"dynarmic/backend/x64/emit_x64.h\"\n#include \"dynarmic/backend/x64/jit_group.h\""
        emitter_header "${emitter_header}")
    string(REPLACE "    A64::Jit* jit_interface = nullptr;"
        "    A64::Jit* jit_interface = nullptr;\n    // ProsperoEden: the process's shared blocks (the JIT sets it), and the guest range and\n    // unlinked branch sites of the last Emit, registered by the JIT.\n    JitGroup* eden_group = nullptr;\n    u64 eden_range_first = 0;\n    u64 eden_range_last = 0;\n    std::vector<JitGroup::PendingSite> eden_pending_sites;"
        emitter_header "${emitter_header}")
endmacro()

# a64_emit_x64.cpp: per-core values from the JIT state, linking through the group, no
# registration in Emit (the JIT registers the block).
macro(eden_shared_jit_emitter)
    set(eden_register "    const auto range = boost::icl::discrete_interval<u64>::closed(descriptor.PC(), end_location.PC() - 1);\n    block_ranges.AddRange(range, descriptor);\n\n    auto bdesc = RegisterBlock(descriptor, entrypoint, size);")
    set(eden_dispatch "        code.mov(r8, u64(fast_dispatch_table.data()));")
    set(eden_get_tpidr "        code.mov(result, u64(conf.tpidr_el0));")
    set(eden_get_tpidrro "        code.mov(result, u64(conf.tpidrro_el0));")
    set(eden_set_tpidr "        code.mov(addr, u64(conf.tpidr_el0));")
    set(eden_push_rsb "    EmitX64::EmitPushRSB(ctx, inst);\n}")
    set(eden_links_begin "bool EmitTerminalImpl(A64EmitX64& e, IR::Term::LinkBlock terminal, IR::LocationDescriptor, bool is_single_step) {")
    set(eden_opcode_call "opcode_branch:\n        (this->*opcode_handlers[size_t(opcode)])(ctx, &inst);")
    set(eden_links_end "bool EmitTerminalImpl(A64EmitX64& e, IR::Term::PopRSBHint, IR::LocationDescriptor, bool is_single_step) {")
    foreach(anchor eden_register eden_dispatch eden_get_tpidr eden_get_tpidrro eden_set_tpidr eden_push_rsb eden_links_begin eden_links_end eden_opcode_call)
        eden_shared_jit_require("${emitter}" "${${anchor}}" "A64 emitter (${anchor})")
    endforeach()
    # A fastmem fault marks its site do-not-fastmem and drops the block from this emitter, but the
    # group keeps the block registered and linked, so it ran and faulted again (61 million faults
    # in a PC race). The JIT's own range invalidation removes it group-wide like a code change.
    set(eden_fastmem_cb "        exception_handler.SetFastmemCallback([this](u64 rip_) {\n            return FastmemCallback(rip_);\n        });")
    eden_shared_jit_require("${emitter}" "${eden_fastmem_cb}" "A64 emitter (fastmem callback)")
    string(PREPEND emitter "#include <mutex>\n#include <optional>\nextern \"C\" void eden_fastmem_demote_block(unsigned long long location) __attribute__((weak));  // fastmem_handler.cpp\nextern \"C\" bool eden_jit_demote(unsigned long long address) __attribute__((weak));  // jit_impl.inc\nnamespace Dynarmic::Backend::X64 { std::mutex& EdenFastmemPatchMutex() noexcept; }  // fastmem_handler.cpp\n")
    # No code is written from the fault handler: upstream's callback unpatched links there, outside
    # the code buffer's write window (EnableWriting asserted, then the heap was corrupt).
    string(REPLACE "${eden_fastmem_cb}" [==[        exception_handler.SetFastmemCallback([this](u64 rip_) {
            // Another core may run this emitter's code (the shared JIT) while this emitter's own core
            // compiles: the site table is read under its lock, and the demotion goes to the shared
            // set (fastmem_handler.cpp), never into this emitter's do_not_fastmem.
            std::optional<FastmemPatchInfo> site;
            const u64 eden_base = std::bit_cast<u64>(this->code.getCode());
            {
                std::lock_guard lock{EdenFastmemPatchMutex()};
                // Lichii's table: sorted by the site's offset in the code buffer (emit_x64_memory.cpp.inc).
                if (rip_ >= eden_base && rip_ - eden_base <= UINT32_MAX) {
                    const u32 offset = u32(rip_ - eden_base);
                    const auto it = std::lower_bound(fastmem_patch_info.begin(), fastmem_patch_info.end(), offset,
                        [](const FastmemPatchInfo& entry, u32 value) { return entry.rip < value; });
                    if (it != fastmem_patch_info.end() && it->rip == offset) site = *it;
                }
            }
            if (!site)
                return FakeCall{};  // not a fastmem site: the fault handler passes it on to the crash report
            const DoNotFastmemMarker eden_marker{site->location, static_cast<unsigned>(site->inst_name)};
            if (site->recompile) {
                // The site, not its whole block: one stray access no longer takes the block's other
                // accesses off fastmem. Key: the block's location with the access's IR index in bits
                // 40-55 (PCs below 2^40), so the saved list can move with the game's code
                // (fastmem_handler.cpp).
                if (eden_fastmem_demote_block)
                    eden_fastmem_demote_block(std::get<0>(eden_marker).Value() ^ (u64{std::get<1>(eden_marker)} << 40));
                // Only the core that faulted stops for it (jit_impl.inc EdenDemote); off a guest core's
                // thread, the invalidation halts every core as before.
                const u64 eden_pc = A64::LocationDescriptor{std::get<0>(eden_marker)}.PC();
                if (!eden_jit_demote || !eden_jit_demote(eden_pc))
                    this->jit_interface->InvalidateCacheRange(eden_pc, 4);
            }
            return FakeCall{.call_rip = eden_base + site->callback, .ret_rip = eden_base + site->resume_rip};
        });]==] emitter "${emitter}")
    string(REPLACE "${eden_register}"
        "    // ProsperoEden: the JIT publishes the block in its JitGroup (a64_interface.cpp).\n    eden_range_first = descriptor.PC();\n    eden_range_last = end_location.PC() - 1;\n    const BlockDescriptor bdesc{entrypoint, size};"
        emitter "${emitter}")
    string(REPLACE "${eden_dispatch}"
        "        code.mov(r8, qword[code.ABI_JIT_PTR + offsetof(A64JitState, eden_fast_dispatch)]);"
        emitter "${emitter}")
    string(REPLACE "${eden_get_tpidr}"
        "        code.mov(result, qword[code.ABI_JIT_PTR + offsetof(A64JitState, eden_tpidr_el0)]);"
        emitter "${emitter}")
    string(REPLACE "${eden_get_tpidrro}"
        "        code.mov(result, qword[code.ABI_JIT_PTR + offsetof(A64JitState, eden_tpidrro_el0)]);"
        emitter "${emitter}")
    string(REPLACE "${eden_set_tpidr}"
        "        code.mov(addr, qword[code.ABI_JIT_PTR + offsetof(A64JitState, eden_tpidr_el0)]);"
        emitter "${emitter}")
    # Common opcodes go through EmitX64 member pointers (the base versions): the return stack
    # push must use A64EmitX64::EmitPushRSB, whose target is linked through the JitGroup.
    string(REPLACE "${eden_opcode_call}"
        "opcode_branch:\n        if (opcode == IR::Opcode::PushRSB)\n            A64EmitX64::EmitPushRSB(ctx, &inst);\n        else\n            (this->*opcode_handlers[size_t(opcode)])(ctx, &inst);"
        emitter "${emitter}")
    file(READ "${EDEN_SHARED_JIT_DIR}/jit_push_rsb.inc" eden_push_rsb_body)
    string(REPLACE "${eden_push_rsb}" "${eden_push_rsb_body}" emitter "${emitter}")
    string(FIND "${emitter}" "${eden_links_begin}" eden_links_at)
    string(FIND "${emitter}" "${eden_links_end}" eden_links_stop)
    if(eden_links_stop LESS eden_links_at)
        message(FATAL_ERROR "Pinned A64 link terminals changed for the shared JIT")
    endif()
    string(SUBSTRING "${emitter}" 0 ${eden_links_at} eden_links_prefix)
    string(SUBSTRING "${emitter}" ${eden_links_stop} -1 eden_links_suffix)
    file(READ "${EDEN_SHARED_JIT_DIR}/jit_links.inc" eden_links)
    set(emitter "${eden_links_prefix}${eden_links}\n${eden_links_suffix}")
endmacro()

# emit_x64_memory.cpp.inc: the running core's monitor slots and configuration.
macro(eden_shared_jit_memory_inc)
    # Fastmem site choice: a site a fault demoted on any core (the shared set, fastmem_handler.cpp)
    # compiles without fastmem on every core, and 64-bit stack accesses (SP based) never use
    # the window: thread stacks are small scattered allocations, never a whole 2 MiB region, so each
    # such site only faulted, was recompiled and faulted again on the next core.
    set(eden_should_fastmem "    const auto marker = std::make_tuple(ctx.Location(), inst->GetName());\n    if (do_not_fastmem.count(marker) > 0) {\n        return std::nullopt;\n    }\n    return marker;\n}")
    eden_shared_jit_require("${memory_source}" "${eden_should_fastmem}" "ShouldFastmem")
    string(REPLACE "${eden_should_fastmem}" [==[    const auto marker = std::make_tuple(ctx.Location(), inst->GetName());
    if (do_not_fastmem.count(marker) > 0) {
        return std::nullopt;
    }
    if (eden_fastmem_block_demoted &&
        eden_fastmem_block_demoted(IR::LocationDescriptor{ctx.Location()}.Value() ^ (u64{inst->GetName()} << 40)))
        return std::nullopt;
    if (!CheckedFastmemSites())  // dev-settings fastmem_sites=off: window kept, no direct accesses
        return std::nullopt;
    if constexpr (sizeof(typename AxxJitState::ProgramCounterType) == 8) {
        if (!eden_fastmem_stack && EdenStackAddress(inst->GetArg(1)))
            return std::nullopt;
    }
    return marker;
}]==] memory_source "${memory_source}")
    # Every registration of a fastmem site takes the lock the fault callback reads the table under.
    string(REPLACE "fastmem_patch_info.emplace(" "EdenLockedPatches<decltype(fastmem_patch_info)>{std::unique_lock{EdenFastmemPatchMutex()}, fastmem_patch_info}.map.emplace(" memory_source "${memory_source}")
    # Lichii's dynarmic (2026-11): every registration goes through AddFastmemPatch; it takes the same lock.
    string(REPLACE "void AxxEmitX64::AddFastmemPatch(CodePtr location, CodePtr resume_location, void (*callback)(), const DoNotFastmemMarker& marker, bool recompile) {"
        "std::mutex& EdenFastmemPatchMutex() noexcept;  // fastmem_handler.cpp\nvoid AxxEmitX64::AddFastmemPatch(CodePtr location, CodePtr resume_location, void (*callback)(), const DoNotFastmemMarker& marker, bool recompile) {\n    std::unique_lock eden_patch_lock{EdenFastmemPatchMutex()};"
        memory_source "${memory_source}")
    set(eden_should_fastmem_head "std::optional<AxxEmitX64::DoNotFastmemMarker> AxxEmitX64::ShouldFastmem(")
    eden_shared_jit_require("${memory_source}" "${eden_should_fastmem_head}" "ShouldFastmem head")
    string(REPLACE "${eden_should_fastmem_head}" [==[extern "C" bool eden_fastmem_block_demoted(unsigned long long location) __attribute__((weak));
extern "C" bool eden_fastmem_stack;            // fastmem_handler.cpp: stack accesses may use the window
std::mutex& EdenFastmemPatchMutex() noexcept;  // fastmem_handler.cpp
bool CheckedFastmemSites() noexcept;           // fastmem_handler.cpp
// A site table, locked for one expression (the temporary lives until its end).
template<typename Map>
struct EdenLockedPatches {
    std::unique_lock<std::mutex> lock;
    Map& map;
};
namespace {
// An address from SP, directly or plus/minus a value (x29-based ones are demoted by their first
// fault: this file is compiled for A32 too, where the A64 register type is incomplete).
[[maybe_unused]] bool EdenStackAddress(const IR::Value& value) {
    if (value.IsImmediate()) return false;
    IR::Inst* const inst = value.GetInst();
    if (inst->GetOpcode() == IR::Opcode::A64GetSP) return true;
    if (inst->GetOpcode() != IR::Opcode::Add64 && inst->GetOpcode() != IR::Opcode::Sub64) return false;
    for (std::size_t i = 0; i < 2; ++i)
        if (!inst->GetArg(i).IsImmediate() && inst->GetArg(i).GetInst()->GetOpcode() == IR::Opcode::A64GetSP) return true;
    return false;
}
} // namespace
std::optional<AxxEmitX64::DoNotFastmemMarker> AxxEmitX64::ShouldFastmem(]==] memory_source "${memory_source}")
    set(eden_address "code.mov(tmp, std::bit_cast<u64>(GetExclusiveMonitorAddressPointer(conf.global_monitor, conf.processor_id)));")
    set(eden_value "code.mov(tmp, std::bit_cast<u64>(GetExclusiveMonitorValuePointer(conf.global_monitor, conf.processor_id)));")
    set(eden_conf_cast "code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(&conf));")
    set(eden_conf_plain "code.mov(code.ABI_PARAM1, u64(&conf));")
    foreach(anchor eden_address eden_value eden_conf_cast eden_conf_plain)
        eden_shared_jit_require("${memory_source}" "${${anchor}}" "exclusive monitor emission (${anchor})")
    endforeach()
    string(REPLACE "${eden_address}" "EDEN_EXCLUSIVE_ADDRESS(tmp);" memory_source "${memory_source}")
    string(REPLACE "${eden_value}" "EDEN_EXCLUSIVE_VALUE(tmp);" memory_source "${memory_source}")
    string(REPLACE "${eden_conf_cast}" "EDEN_CONF_ARG();" memory_source "${memory_source}")
    string(REPLACE "${eden_conf_plain}" "EDEN_CONF_ARG();" memory_source "${memory_source}")
endmacro()

# emit_x64_memory.h: A64 code may run on any core, so an exclusive store clears every
# reservation of its address (its own included, which the store consumes anyway).
macro(eden_shared_jit_memory_header)
    set(eden_skip_own "        if (processor_index == conf.processor_id) {\n            continue;\n        }")
    eden_shared_jit_require("${memory_emitter}" "${eden_skip_own}" "exclusive reservation clearing")
    string(REPLACE "${eden_skip_own}"
        "        if constexpr (!requires(const UserConfig& c) { c.tpidr_el0; }) {\n            if (processor_index == conf.processor_id) {\n                continue;\n            }\n        }"
        memory_emitter "${memory_emitter}")
endmacro()

# a32/a64_emit_x64_memory.cpp: what the macros above load.
macro(eden_shared_jit_memory_unit bits)
    if(${bits} EQUAL 64)
        string(PREPEND memory_source "#define EDEN_EXCLUSIVE_ADDRESS(reg) code.mov(reg, code.qword[code.ABI_JIT_PTR + offsetof(A64JitState, eden_exclusive_address)])\n#define EDEN_EXCLUSIVE_VALUE(reg) code.mov(reg, code.qword[code.ABI_JIT_PTR + offsetof(A64JitState, eden_exclusive_value)])\n#define EDEN_CONF_ARG() code.mov(code.ABI_PARAM1, code.qword[code.ABI_JIT_PTR + offsetof(A64JitState, eden_conf)])\n")
    else()
        string(PREPEND memory_source "#define EDEN_EXCLUSIVE_ADDRESS(reg) code.mov(reg, std::bit_cast<u64>(GetExclusiveMonitorAddressPointer(conf.global_monitor, conf.processor_id)))\n#define EDEN_EXCLUSIVE_VALUE(reg) code.mov(reg, std::bit_cast<u64>(GetExclusiveMonitorValuePointer(conf.global_monitor, conf.processor_id)))\n#define EDEN_CONF_ARG() code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(&conf))\n")
    endif()
endmacro()

# a64_interface.cpp: upstream's Impl replaced by jit_impl.inc, with the group helpers before it.
macro(eden_shared_jit_interface)
    set(eden_impl_begin "struct Jit::Impl final {")
    set(eden_impl_end "Jit::Jit(UserConfig conf)")
    string(FIND "${jit_source}" "${eden_impl_begin}" eden_impl_at)
    string(FIND "${jit_source}" "${eden_impl_end}" eden_impl_stop)
    if(eden_impl_at LESS 0 OR eden_impl_stop LESS eden_impl_at)
        message(FATAL_ERROR "Pinned A64 JIT implementation changed for the shared JIT")
    endif()
    string(SUBSTRING "${jit_source}" 0 ${eden_impl_at} eden_impl_prefix)
    string(SUBSTRING "${jit_source}" ${eden_impl_stop} -1 eden_impl_suffix)
    file(READ "${EDEN_SHARED_JIT_DIR}/jit_group_support.inc" eden_group_support)
    file(READ "${EDEN_SHARED_JIT_DIR}/jit_impl.inc" eden_impl)
    set(jit_source "${eden_impl_prefix}${eden_group_support}\n${eden_impl}\n${eden_impl_suffix}")
    string(PREPEND jit_source "#include <algorithm>\n#include <atomic>\n#include <chrono>\n#include <cstddef>\n#include <cstdio>\n#include <memory>\n#include <thread>\n#include <vector>\n#include \"dynarmic/backend/x64/exclusive_monitor_friend.h\"\n#include \"dynarmic/backend/x64/jit_group.h\"\nextern \"C\" void eden_jit_phases(unsigned core, unsigned long long translate, unsigned long long optimize, unsigned long long emit, unsigned long long location) __attribute__((weak));\nextern \"C\" unsigned long long eden_jit_clock_ns() __attribute__((weak));\n")
endmacro()

# The JIT state layout, the group header and the callback argument registry, all compiled for
# every dynarmic object (the definition recompiles objects that saw the upstream layout).
macro(eden_shared_jit_sources)
    set(eden_jitstate_relative "dynarmic/backend/x64/a64_jitstate.h")
    file(READ "${PROJECT_SOURCE_DIR}/src/dynarmic/src/${eden_jitstate_relative}" eden_jitstate)
    set(eden_jitstate_anchor "    std::array<u64, RSB_SIZE> rsb_codeptrs;\n")
    eden_shared_jit_require("${eden_jitstate}" "${eden_jitstate_anchor}" "A64 JIT state")
    string(REPLACE "${eden_jitstate_anchor}" "${eden_jitstate_anchor}
    // ProsperoEden: what generated code reads instead of embedding it, so that any core can run
    // code another core compiled (jit_group.h): this core's callbacks and JIT object, TPIDR
    // registers, exclusive monitor slots, configuration and fast dispatch table.
    void* eden_callbacks = nullptr;
    void* eden_jit = nullptr;
    u64* eden_tpidr_el0 = nullptr;
    const u64* eden_tpidrro_el0 = nullptr;
    void* eden_exclusive_address = nullptr;
    void* eden_exclusive_value = nullptr;
    const void* eden_conf = nullptr;
    void* eden_fast_dispatch = nullptr;
" eden_jitstate "${eden_jitstate}")
    write_derived("${PORT_BUILD_DIR}/include/${eden_jitstate_relative}" "${eden_jitstate}")
    file(READ "${EDEN_SHARED_JIT_DIR}/jit_group.h" eden_group_header)
    write_derived("${PORT_BUILD_DIR}/include/dynarmic/backend/x64/jit_group.h" "${eden_group_header}")
    file(READ "${PROJECT_SOURCE_DIR}/src/dynarmic/src/dynarmic/backend/x64/callback.cpp" eden_callback)
    set(eden_call_arg "    code.mov(code.ABI_PARAM1, arg);\n    code.CallFunction(fn);\n}")
    set(eden_return_arg "    code.mov(code.ABI_PARAM2, arg);\n#endif")
    set(eden_callback_namespace "namespace Dynarmic::Backend::X64 {\n")
    foreach(anchor eden_call_arg eden_return_arg eden_callback_namespace)
        eden_shared_jit_require("${eden_callback}" "${${anchor}}" "callback emission (${anchor})")
    endforeach()
    file(READ "${EDEN_SHARED_JIT_DIR}/jit_state_args.inc" eden_state_args)
    string(REPLACE "${eden_callback_namespace}" "${eden_callback_namespace}${eden_state_args}" eden_callback "${eden_callback}")
    string(REPLACE "${eden_call_arg}" "    EdenLoadArg(code, code.ABI_PARAM1, arg);\n    code.CallFunction(fn);\n}" eden_callback "${eden_callback}")
    string(REPLACE "${eden_return_arg}" "    EdenLoadArg(code, code.ABI_PARAM2, arg);\n#endif" eden_callback "${eden_callback}")
    write_derived("${PORT_BUILD_DIR}/callback.cpp" "#include <array>\n#include <atomic>\n#include <cstdlib>\n#include <optional>\n${eden_callback}")
    get_target_property(eden_jit_sources dynarmic SOURCES)
    list(FILTER eden_jit_sources EXCLUDE REGEX "(^|/)callback\\.cpp$")
    set_property(TARGET dynarmic PROPERTY SOURCES "${eden_jit_sources}")
    target_sources(dynarmic PRIVATE "${PORT_BUILD_DIR}/callback.cpp")
    target_compile_definitions(dynarmic PRIVATE EDEN_SHARED_JIT_LAYOUT=1)
endmacro()
