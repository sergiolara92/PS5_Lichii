// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/loader/development_factory.h"

#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include "core/loader/loader.h"

namespace Loader {
namespace {
struct DevelopmentRegistry {
    std::mutex mutex;
    DevelopmentFactory factory;
    u64 sequence = 0;
    u64 owner = 0;
};
DevelopmentRegistry& Registry() {
    static DevelopmentRegistry registry;
    return registry;
}
} // namespace

u64 InstallDevelopmentFactory(DevelopmentFactory factory) {
    auto& registry = Registry();
    DevelopmentFactory displaced;
    u64 token;
    {
        std::lock_guard lock{registry.mutex};
        if (registry.sequence == std::numeric_limits<u64>::max())
            throw std::overflow_error("Development factory ownership tokens exhausted");
        token = ++registry.sequence;
        displaced.swap(registry.factory);
        registry.factory.swap(factory);
        registry.owner = token;
    }
    // Captured objects may reenter the registry when destroyed.
    return token;
}

bool RemoveDevelopmentFactory(u64 token) {
    auto& registry = Registry();
    DevelopmentFactory displaced;
    {
        std::lock_guard lock{registry.mutex};
        if (token == 0 || token != registry.owner) return false;
        displaced.swap(registry.factory);
        registry.owner = 0;
    }
    return true;
}

std::unique_ptr<AppLoader> TryDevelopmentFactory(Core::System& system,
                                                FileSys::VirtualFile file,
                                                u64 program_id,
                                                std::size_t program_index) {
    if (!file) return {};
    DevelopmentFactory factory;
    {
        auto& registry = Registry();
        std::lock_guard lock{registry.mutex};
        factory = registry.factory;
    }
    return factory ? factory(system, std::move(file), program_id, program_index) : nullptr;
}
} // namespace Loader
