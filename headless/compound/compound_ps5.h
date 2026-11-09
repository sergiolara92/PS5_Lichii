/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PROSPERO_COMPOUND_PS5_H
#define PROSPERO_COMPOUND_PS5_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COMPOUND_PS5_ABI_VERSION 1U

enum compound_ps5_status {
    COMPOUND_PS5_OK = 0,
    COMPOUND_PS5_INVALID_ARGUMENT = 1,
    COMPOUND_PS5_SELFTEST_FAILED = 2
};

/* Fixed-width fields and caller-supplied sizes keep the boundary usable from C/C++.
 * String pointers refer to immutable static storage; callers must not free them. */
typedef struct compound_ps5_capabilities {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t integer_a64_interpreter;
    uint32_t floating_point_simd_atomics;
    uint32_t jit;
    uint32_t gpu_renderer;
    uint32_t multiple_executable_modules;
    uint32_t switch1_commercial_launch;
    uint32_t switch2_commercial_launch;
    uint32_t reserved;
    const char *upstream_commit;
    const char *unavailable_reason;
} compound_ps5_capabilities;

typedef struct compound_ps5_selftest_report {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t checks_total;
    uint32_t checks_passed;
    uint32_t instructions_retired;
    uint32_t trap_kind;
    uint64_t final_x0;
    uint64_t stored_value;
    uint64_t trap_pc;
    char detail[256];
} compound_ps5_selftest_report;

/* Return INVALID_ARGUMENT without writing when the pointer is NULL or its size
 * is too small. Larger caller buffers are accepted; bytes beyond v1 stay intact. */
int compound_ps5_get_capabilities(compound_ps5_capabilities *out, size_t out_size);
/* Execute original Compound interpreter code, including its failure paths.
 * OK means the bounded CPU self-test passed; it does not mean a title can boot. */
int compound_ps5_run_selftest(compound_ps5_selftest_report *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
