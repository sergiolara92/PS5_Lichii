/* SPDX-License-Identifier: GPL-3.0-only */
#include "compound_ps5.h"
#include "cpu/interpreter.h"

#include <stdio.h>
#include <string.h>

static unsigned failures;

static void require(int condition, const char *name)
{
    if (!condition) {
        ++failures;
        fprintf(stderr, "FAIL: %s\n", name);
    }
}

int main(void)
{
    compound_ps5_capabilities capabilities;
    memset(&capabilities, 0xA5, sizeof(capabilities));
    const compound_ps5_capabilities sentinel = capabilities;
    require(compound_ps5_get_capabilities(NULL, sizeof(capabilities)) ==
                COMPOUND_PS5_INVALID_ARGUMENT, "NULL capability output rejected");
    require(compound_ps5_get_capabilities(&capabilities, sizeof(capabilities) - 1U) ==
                COMPOUND_PS5_INVALID_ARGUMENT, "undersized capability output rejected");
    require(memcmp(&capabilities, &sentinel, sizeof(capabilities)) == 0,
            "undersized capability output left intact");
    require(compound_ps5_get_capabilities(&capabilities, sizeof(capabilities)) ==
                COMPOUND_PS5_OK, "capability ABI call succeeds");
    require(capabilities.struct_size == sizeof(capabilities) &&
                capabilities.abi_version == COMPOUND_PS5_ABI_VERSION,
            "capability ABI version/size match");
    require(capabilities.integer_a64_interpreter == 1U &&
                capabilities.floating_point_simd_atomics == 0U &&
                capabilities.jit == 0U && capabilities.gpu_renderer == 0U &&
                capabilities.multiple_executable_modules == 0U &&
                capabilities.switch1_commercial_launch == 0U &&
                capabilities.switch2_commercial_launch == 0U,
            "capabilities disclose bounded CPU-only scope");
    require(strcmp(capabilities.upstream_commit,
                   "9ea3122589f1116838a5f2bef46dc5be9a79cba8") == 0,
            "capability source revision matches vendored original");
    require(strstr(capabilities.unavailable_reason, "unavailable") != NULL,
            "capability diagnostic explains unavailable title launching");

    struct extended_report {
        compound_ps5_selftest_report report;
        unsigned char tail[16];
    } output;
    memset(&output, 0xA5, sizeof(output));
    unsigned char snapshot[sizeof(output)];
    memcpy(snapshot, &output, sizeof(output));
    require(compound_ps5_run_selftest(NULL, sizeof(output.report)) ==
                COMPOUND_PS5_INVALID_ARGUMENT, "NULL self-test output rejected");
    require(compound_ps5_run_selftest(&output.report, sizeof(output.report) - 1U) ==
                COMPOUND_PS5_INVALID_ARGUMENT, "undersized self-test output rejected");
    require(memcmp(snapshot, &output, sizeof(output)) == 0,
            "undersized self-test output left intact");
    for (unsigned iteration = 0; iteration < 2U; ++iteration) {
        require(compound_ps5_run_selftest(&output.report, sizeof(output)) == COMPOUND_PS5_OK,
                "original A64 interpreter self-test passes repeatedly");
        require(output.report.abi_version == COMPOUND_PS5_ABI_VERSION &&
                    output.report.struct_size == sizeof(output.report), "report ABI matches");
        require(output.report.checks_total >= 20U &&
                    output.report.checks_passed == output.report.checks_total,
                "all guest execution and rejection checks pass");
        require(output.report.final_x0 == 42U && output.report.stored_value == 42U &&
                    output.report.instructions_retired == 9U &&
                    output.report.trap_kind == INTERPRETER_TRAP_SVC &&
                    output.report.trap_pc == UINT64_C(0x40000030),
                "observed execution matches independent guest result");
        require(output.report.detail[sizeof(output.report.detail) - 1U] == '\0',
                "report detail is bounded and terminated");
    }
    for (size_t byte = 0; byte < sizeof(output.tail); ++byte) {
        require(output.tail[byte] == 0xA5U, "v1 ABI preserves extension bytes");
    }

    /* Independent guest: MOVZ X0,#7; MUL X2,X0,X0; BRK #0.
     * Verify a different arithmetic family and debugger trap directly against
     * the vendored interpreter, beyond the wrapper's demonstration program. */
    POUND_ALIGNED(64) uint8_t code[64] = {
        0xE0, 0x00, 0x80, 0xD2,
        0x02, 0x7C, 0x00, 0x9B,
        0x00, 0x00, 0x20, 0xD4,
    };
    guest_memory_t memory;
    guest_state_t state;
    interpreter_trap_t trap;
    uint32_t retired = 0;
    require(guest_memory_init(&memory, code, sizeof(code), UINT64_C(0x10000)) ==
                POUND_SUCCESS, "independent guest mapping succeeds");
    require(guest_state_init(&state) == POUND_SUCCESS, "independent guest state initializes");
    state.pc = UINT64_C(0x10000);
    require(interpreter_run(&state, &memory, 8U, &retired, &trap) == POUND_SUCCESS &&
                state.x[2] == 49U && retired == 2U &&
                trap.kind == INTERPRETER_TRAP_BREAKPOINT &&
                state.pc == UINT64_C(0x10008),
            "independent multiply program executes and traps");
    require(guest_memory_init(&memory, code + 1U, sizeof(code) - 1U,
                              UINT64_C(0x10000)) == POUND_ERROR_MEMORY_ALIGNMENT,
            "misaligned guest backing memory rejected");

    printf("%s\n", output.report.detail);
    if (failures != 0U) {
        fprintf(stderr, "%u host assertions failed.\n", failures);
        return 1;
    }
    puts("PASS: capability ABI, output bounds, repeatability and independent A64 multiply guest.");
    return 0;
}
