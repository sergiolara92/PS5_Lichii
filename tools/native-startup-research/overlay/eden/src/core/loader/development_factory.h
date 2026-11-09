// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace Core { class System; }

namespace Loader {
class AppLoader;

// An explicitly installed development hook. Returning null leaves ordinary
// loader resolution intact. Callback exceptions propagate to the caller.
using DevelopmentFactory = std::function<std::unique_ptr<AppLoader>(
    Core::System&, FileSys::VirtualFile, u64 program_id, std::size_t program_index)>;

// Each install replaces the previous hook and returns a nonzero ownership token.
// Installing an empty factory clears behavior but still creates a new lease.
u64 InstallDevelopmentFactory(DevelopmentFactory factory);
// An old lease cannot remove a factory installed after it.
bool RemoveDevelopmentFactory(u64 token);
// Snapshot the callback while locked, then invoke outside the registry mutex.
// A snapshot may finish after its lease is removed; removal is not cancellation.
std::unique_ptr<AppLoader> TryDevelopmentFactory(Core::System& system,
                                                FileSys::VirtualFile file,
                                                u64 program_id,
                                                std::size_t program_index);
} // namespace Loader
