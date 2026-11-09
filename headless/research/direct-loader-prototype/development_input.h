// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "core/file_sys/vfs/vfs.h"
#include "core/loader/loader.h"

namespace Prospero::Development {

// Explicit development inputs only. No filename guess, global loader override,
// game byte mutation, or production launcher capability is installed.
struct Inputs {
    // Optional backing filesystem/archive lifetime, retained by exported images.
    std::shared_ptr<void> lifetime_owner;
    FileSys::VirtualDir exefs;
    FileSys::VirtualDir romfs_directory;
    FileSys::VirtualFile romfs_image;
    std::vector<std::string> required_modules{"main"};
};

struct InputReport {
    Loader::ResultStatus status = Loader::ResultStatus::ErrorNotInitialized;
    std::string detail;
    std::vector<std::string> modules;
};

// This is bounded structural preflight, not NPDM authentication or NSO loading.
InputReport ValidateInputs(const Inputs& inputs);
FileSys::VirtualFile MainWithExplicitParent(const Inputs& inputs);

class LazyRomFs {
public:
    explicit LazyRomFs(const Inputs& inputs);
    Loader::ResultStatus Read(FileSys::VirtualFile& output);
private:
    std::shared_ptr<void> owner; // Outlives source/image destruction.
    FileSys::VirtualDir directory;
    FileSys::VirtualFile image;
    std::once_flag built;
};

// A testable ownership seam: only the explicit top-level wrapper invokes this.
// A rejected registration never consumes the one-time gate.
class RegistrationGate {
public:
    bool Commit(u64 process_id, u64 title_id, const std::function<bool(u64, u64)>& register_owner);
    bool Registered() const { return registered; }
private:
    bool registered = false;
};

} // namespace Prospero::Development
