// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#ifdef PROSPERO_ENABLE_COMPOUND
#include "compound_ps5.h"
#endif

namespace Prospero {

// Emulator cores and graphics renderers are separate concepts. Compound's current
// bridge only exposes a bounded CPU diagnostic; Eden remains the game backend.
struct CompoundDiagnostic {
    bool linked = false;
    bool passed = false;
    std::uint32_t checks_passed = 0;
    std::uint32_t checks_total = 0;
    std::uint32_t instructions_retired = 0;
    std::string upstream_commit;
    std::string detail;
};

inline const CompoundDiagnostic& GetCompoundDiagnostic() {
    // The About screen and startup log share one actual execution of the test.
    static const CompoundDiagnostic diagnostic = [] {
        CompoundDiagnostic value;
#ifdef PROSPERO_ENABLE_COMPOUND
        value.linked = true;
        compound_ps5_capabilities capabilities{};
        if (compound_ps5_get_capabilities(&capabilities, sizeof(capabilities)) != COMPOUND_PS5_OK ||
            capabilities.abi_version != COMPOUND_PS5_ABI_VERSION) {
            value.detail = "Compound capability ABI check failed";
            return value;
        }
        if (capabilities.upstream_commit) value.upstream_commit = capabilities.upstream_commit;
        compound_ps5_selftest_report report{};
        value.passed = compound_ps5_run_selftest(&report, sizeof(report)) == COMPOUND_PS5_OK &&
                       report.abi_version == COMPOUND_PS5_ABI_VERSION && report.checks_total != 0 &&
                       report.checks_passed == report.checks_total;
        value.checks_passed = report.checks_passed;
        value.checks_total = report.checks_total;
        value.instructions_retired = report.instructions_retired;
        const char* detail_end = report.detail;
        while (detail_end != report.detail + sizeof(report.detail) && *detail_end) ++detail_end;
        value.detail.assign(report.detail, static_cast<std::size_t>(detail_end - report.detail));
#else
        value.detail = "Compound was disabled at build time";
#endif
        return value;
    }();
    return diagnostic;
}

inline void LogCompoundDiagnostic(std::FILE* output = stderr) {
    const auto& diagnostic = GetCompoundDiagnostic();
    std::fprintf(output,
                 "PROSPERO_COMPOUND linked=%u cpu_selftest=%s checks=%u/%u instructions=%u "
                 "upstream=%s switch2_launch=unavailable detail=%s\n",
                 static_cast<unsigned>(diagnostic.linked),
                 diagnostic.linked ? (diagnostic.passed ? "PASS" : "FAIL") : "DISABLED",
                 diagnostic.checks_passed, diagnostic.checks_total, diagnostic.instructions_retired,
                 diagnostic.upstream_commit.c_str(), diagnostic.detail.c_str());
}

} // namespace Prospero
