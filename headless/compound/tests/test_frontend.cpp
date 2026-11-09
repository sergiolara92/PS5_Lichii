// SPDX-License-Identifier: GPL-3.0-only
#include "../../compound_diagnostics.h"

#include <cstdio>
#include <string>

int main() {
    const auto& first = Prospero::GetCompoundDiagnostic();
    const auto& second = Prospero::GetCompoundDiagnostic();
    if (&first != &second) {
        std::fputs("FAIL: frontend diagnostic cache changed instance\n", stderr);
        return 1;
    }
#ifdef PROSPERO_ENABLE_COMPOUND
    if (!first.linked || !first.passed || first.checks_total < 20 ||
        first.checks_passed != first.checks_total || first.instructions_retired != 9 ||
        first.upstream_commit != "9ea3122589f1116838a5f2bef46dc5be9a79cba8") {
        std::fprintf(stderr, "FAIL: enabled frontend CPU diagnostic: %s\n", first.detail.c_str());
        return 1;
    }
#else
    if (first.linked || first.passed || first.checks_total != 0 ||
        first.detail != "Compound was disabled at build time") {
        std::fputs("FAIL: disabled frontend unexpectedly claims Compound availability\n", stderr);
        return 1;
    }
#endif

#ifdef _WIN32
    std::FILE* output = nullptr;
    (void)::tmpfile_s(&output);
#else
    std::FILE* output = std::tmpfile();
#endif
    if (!output) {
        std::fputs("FAIL: diagnostic capture could not open temporary file\n", stderr);
        return 1;
    }
    Prospero::LogCompoundDiagnostic(output);
    std::rewind(output);
    char line[1024]{};
    const bool read = std::fgets(line, sizeof(line), output) != nullptr;
    std::fclose(output);
    const std::string log(line);
    if (!read || log.find("PROSPERO_COMPOUND") == std::string::npos ||
        log.find("switch2_launch=unavailable") == std::string::npos) {
        std::fputs("FAIL: frontend log omitted capability limitation\n", stderr);
        return 1;
    }
#ifdef PROSPERO_ENABLE_COMPOUND
    if (log.find("linked=1 cpu_selftest=PASS") == std::string::npos ||
        log.find(first.upstream_commit) == std::string::npos) {
        std::fputs("FAIL: enabled frontend log omitted CPU result/source revision\n", stderr);
        return 1;
    }
#else
    if (log.find("linked=0 cpu_selftest=DISABLED") == std::string::npos) {
        std::fputs("FAIL: disabled frontend log is incorrect\n", stderr);
        return 1;
    }
#endif
    std::puts(line);
    return 0;
}
