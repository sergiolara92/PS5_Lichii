/* SPDX-License-Identifier: GPL-3.0-only */
#include "compound_ps5.h"

#include "cpu/interpreter.h"
#include "guest_state.h"
#include "memory/guest_memory.h"

#include <stdio.h>
#include <string.h>

#define COMPOUND_GUEST_BASE UINT64_C(0x40000000)
#define COMPOUND_TEST_MEMORY_SIZE 4096U

typedef struct compound_test_machine {
    POUND_ALIGNED(64) uint8_t bytes[COMPOUND_TEST_MEMORY_SIZE];
    guest_memory_t memory;
    guest_state_t state;
} compound_test_machine;

static int reset_machine(compound_test_machine *machine)
{
    memset(machine, 0, sizeof(*machine));
    if (guest_memory_init(&machine->memory, machine->bytes, sizeof(machine->bytes),
                          COMPOUND_GUEST_BASE) != POUND_SUCCESS ||
        guest_state_init(&machine->state) != POUND_SUCCESS) {
        return 0;
    }
    machine->state.pc = COMPOUND_GUEST_BASE;
    return 1;
}

static void put_word(compound_test_machine *machine, size_t offset, uint32_t word)
{
    for (unsigned byte = 0; byte < 4U; ++byte) {
        machine->bytes[offset + byte] = (uint8_t)(word >> (8U * byte));
    }
}

static uint64_t read_u64(const uint8_t *bytes)
{
    uint64_t value = 0;
    for (unsigned byte = 0; byte < 8U; ++byte) {
        value |= (uint64_t)bytes[byte] << (8U * byte);
    }
    return value;
}

static void check(compound_ps5_selftest_report *report, int passed, const char *name)
{
    ++report->checks_total;
    if (passed) {
        ++report->checks_passed;
    } else if (report->detail[0] == '\0') {
        (void)snprintf(report->detail, sizeof(report->detail), "Failed: %s", name);
    }
}

int compound_ps5_get_capabilities(compound_ps5_capabilities *out, size_t out_size)
{
    if (out == NULL || out_size < sizeof(*out)) {
        return COMPOUND_PS5_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    out->abi_version = COMPOUND_PS5_ABI_VERSION;
    out->integer_a64_interpreter = 1U;
    out->upstream_commit = "9ea3122589f1116838a5f2bef46dc5be9a79cba8";
    out->unavailable_reason =
        "Compound provides a bounded A64 integer interpreter self-test here. "
        "Switch 1 and Switch 2 title launching is unavailable: executable loading, "
        "complete CPU/OS services and a guest GPU renderer are not integrated. "
        "FP/SIMD/atomics, JIT and multiple executable modules are unsupported.";
    return COMPOUND_PS5_OK;
}

int compound_ps5_run_selftest(compound_ps5_selftest_report *out, size_t out_size)
{
    if (out == NULL || out_size < sizeof(*out)) {
        return COMPOUND_PS5_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    out->abi_version = COMPOUND_PS5_ABI_VERSION;

    compound_test_machine machine;
    if (!reset_machine(&machine)) {
        check(out, 0, "guest memory/register initialization");
        return COMPOUND_PS5_SELFTEST_FAILED;
    }

    /* Literal A64 encodings, not native host instructions. Every branch skips a
     * destructive MOV, so arithmetic and all three branch shapes are observed.
     * The SVC is reported by the core; no Horizon service is simulated here. */
    const uint32_t program[] = {
        UINT32_C(0xD28002A0), /* MOVZ X0, #21 */
        UINT32_C(0x91005400), /* ADD X0, X0, #21 */
        UINT32_C(0xF9000020), /* STR X0, [X1] */
        UINT32_C(0xF9400022), /* LDR X2, [X1] */
        UINT32_C(0xEB02001F), /* CMP X0, X2 */
        UINT32_C(0x54000040), /* B.EQ +8 */
        UINT32_C(0xD2800000), /* MOVZ X0, #0 (must be skipped) */
        UINT32_C(0xD1000843), /* SUB X3, X2, #2 */
        UINT32_C(0xB5000043), /* CBNZ X3, +8 */
        UINT32_C(0xD2800000), /* MOVZ X0, #0 (must be skipped) */
        UINT32_C(0x14000002), /* B +8 */
        UINT32_C(0xD2800000), /* MOVZ X0, #0 (must be skipped) */
        UINT32_C(0xD4000541), /* SVC #42 */
    };
    for (size_t word = 0; word < sizeof(program) / sizeof(program[0]); ++word) {
        put_word(&machine, word * 4U, program[word]);
    }
    machine.state.x[1] = COMPOUND_GUEST_BASE + 0x100U;
    uint32_t retired = 0;
    interpreter_trap_t trap;
    error_t status = interpreter_run(&machine.state, &machine.memory, 32U, &retired, &trap);
    out->instructions_retired = retired;
    out->trap_kind = (uint32_t)trap.kind;
    out->final_x0 = machine.state.x[0];
    out->stored_value = read_u64(machine.bytes + 0x100U);
    out->trap_pc = trap.pc;
    check(out, status == POUND_SUCCESS, "arithmetic/branch/load/store execution status");
    check(out, machine.state.x[0] == 42U, "addition and taken branches");
    check(out, machine.state.x[2] == 42U, "64-bit load");
    check(out, machine.state.x[3] == 40U, "subtraction");
    check(out, out->stored_value == 42U, "64-bit store");
    check(out, retired == 9U, "exact retired instruction count");
    check(out, trap.kind == INTERPRETER_TRAP_SVC && trap.imm == 42U,
          "expected SVC trap and immediate");
    check(out, machine.state.pc == COMPOUND_GUEST_BASE + 48U &&
                   trap.pc == machine.state.pc, "SVC leaves PC at trap");

    /* Zero budget is a valid no-op in the upstream API. A bounded loop must
     * retire precisely its budget instead of escaping into native code. */
    if (!reset_machine(&machine)) {
        check(out, 0, "budget test initialization");
        return COMPOUND_PS5_SELFTEST_FAILED;
    }
    put_word(&machine, 0U, UINT32_C(0x14000000)); /* B . */
    status = interpreter_run(&machine.state, &machine.memory, 0U, &retired, &trap);
    check(out, status == POUND_SUCCESS && retired == 0U &&
                   machine.state.pc == COMPOUND_GUEST_BASE &&
                   trap.kind == INTERPRETER_TRAP_NONE, "zero budget is a no-op");
    status = interpreter_run(&machine.state, &machine.memory, 4U, &retired, &trap);
    check(out, status == POUND_SUCCESS && retired == 4U &&
                   machine.state.pc == COMPOUND_GUEST_BASE &&
                   trap.kind == INTERPRETER_TRAP_NONE, "loop stops at instruction budget");
    check(out, interpreter_run(NULL, &machine.memory, 1U, &retired, &trap) ==
                   POUND_ERROR_INVALID_ARGUMENT, "NULL register state is rejected");
    check(out, interpreter_run(&machine.state, NULL, 1U, &retired, &trap) ==
                   POUND_ERROR_INVALID_ARGUMENT, "NULL guest memory is rejected");
    check(out, interpreter_run(&machine.state, &machine.memory, 1U, NULL, &trap) ==
                   POUND_ERROR_INVALID_ARGUMENT, "NULL retirement counter is rejected");

    machine.state.pc = COMPOUND_GUEST_BASE + COMPOUND_TEST_MEMORY_SIZE;
    status = interpreter_run(&machine.state, &machine.memory, 1U, &retired, &trap);
    check(out, status == POUND_ERROR_MEMORY_FAULT && retired == 0U,
          "instruction fetch outside guest window is rejected");
    machine.state.pc = COMPOUND_GUEST_BASE + COMPOUND_TEST_MEMORY_SIZE - 2U;
    status = interpreter_run(&machine.state, &machine.memory, 1U, &retired, &trap);
    check(out, status == POUND_ERROR_MEMORY_FAULT && retired == 0U,
          "instruction fetch crossing guest window is rejected");

    machine.state.pc = COMPOUND_GUEST_BASE;
    put_word(&machine, 0U, UINT32_C(0xF9000020)); /* STR X0, [X1] */
    machine.state.x[0] = UINT64_C(0x123456789ABCDEF0);
    machine.state.x[1] = COMPOUND_GUEST_BASE + COMPOUND_TEST_MEMORY_SIZE - 4U;
    status = interpreter_run(&machine.state, &machine.memory, 1U, &retired, &trap);
    check(out, status == POUND_ERROR_MEMORY_FAULT && retired == 0U &&
                   read_u64(machine.bytes + COMPOUND_TEST_MEMORY_SIZE - 8U) == 0U,
          "64-bit store crossing guest window is rejected without a partial write");

    const guest_memory_region_t read_only = {
        .base = COMPOUND_GUEST_BASE,
        .size = COMPOUND_TEST_MEMORY_SIZE,
        .permission = GUEST_MEMORY_PERM_READ,
    };
    machine.memory.regions = &read_only;
    machine.memory.region_count = 1U;
    machine.state.pc = COMPOUND_GUEST_BASE;
    status = interpreter_run(&machine.state, &machine.memory, 1U, &retired, &trap);
    check(out, status == POUND_ERROR_MEMORY_FAULT && retired == 0U,
          "read-only guest region denies instruction fetch");
    machine.memory.regions = NULL;
    machine.memory.region_count = 0U;

    put_word(&machine, 0U, 0U); /* Undefined A64 encoding. */
    status = interpreter_run(&machine.state, &machine.memory, 1U, &retired, &trap);
    check(out, status == POUND_SUCCESS && retired == 0U &&
                   trap.kind == INTERPRETER_TRAP_UNDEFINED &&
                   machine.state.pc == COMPOUND_GUEST_BASE,
          "undefined encoding traps without retiring");
    check(out, interpreter_probe(UINT32_C(0x1E202800)) == 0,
          "unsupported floating-point instruction is refused");
    check(out, interpreter_probe(UINT32_C(0x4E208400)) == 0,
          "unsupported SIMD instruction is refused");

    if (out->checks_passed != out->checks_total) {
        return COMPOUND_PS5_SELFTEST_FAILED;
    }
    (void)snprintf(out->detail, sizeof(out->detail),
                   "PASS: %u checks; original Compound A64 core retired %u instructions, "
                   "stored 42 and trapped at SVC #42. Title launching is unavailable.",
                   (unsigned)out->checks_passed, (unsigned)out->instructions_retired);
    return COMPOUND_PS5_OK;
}
