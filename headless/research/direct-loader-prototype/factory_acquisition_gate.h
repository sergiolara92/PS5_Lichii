// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <mutex>
#include <stdexcept>

namespace Prospero::Development {

// Pure host-side policy for one explicitly selected loader acquisition. Pointer
// identities are compared only; this gate never dereferences a Core::System.
class FactoryAcquisitionGate {
public:
    enum class Decision { Ignore, Create };

    explicit FactoryAcquisitionGate(const void* expected) : expected_identity(expected) {
        if (!expected_identity)
            throw std::invalid_argument("Explicit factory requires a System identity");
    }

    Decision Acquire(const void* current, bool exact_input, bool valid_program_request) {
        if (current != expected_identity) return Decision::Ignore;
        const std::lock_guard lock(mutex);
        if (!acquired) {
            if (!exact_input || !valid_program_request)
                throw std::runtime_error("Explicit development factory acquisition did not match; refusing ordinary loader fallback");
            acquired = true;
            return Decision::Create;
        }
        if (exact_input)
            throw std::runtime_error("Explicit development factory input was already acquired");
        return Decision::Ignore;
    }

private:
    const void* const expected_identity;
    std::mutex mutex;
    bool acquired = false;
};

} // namespace Prospero::Development
