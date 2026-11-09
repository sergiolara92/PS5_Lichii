// SPDX-License-Identifier: GPL-3.0-or-later
#include "direct_development_loader.h"

#include <utility>
#include "core/core.h"
#include "core/file_sys/romfs_factory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"

namespace Prospero::Development {

DirectLoader::DirectLoader(Inputs inputs_)
    : AppLoader(MainWithExplicitParent(inputs_)), inputs(std::move(inputs_)),
      preflight(ValidateInputs(inputs)), romfs(inputs) {
    if (preflight.status == Loader::ResultStatus::Success) {
        // Its file constructor reads the actual title metadata. The parent proxy
        // ensures it uses this explicit ExeFS, even for ZIP-backed virtual files.
        secondary = std::make_unique<Loader::AppLoader_DeconstructedRomDirectory>(file, false);
    }
}

Loader::AppLoader::LoadResult DirectLoader::Load(Kernel::KProcess& process, Core::System& system) {
    if (attempted_load) return {Loader::ResultStatus::ErrorAlreadyLoaded, {}};
    if (!secondary) return {preflight.status, {}};
    attempted_load = true;
    auto result = secondary->Load(process, system);
    if (result.first != Loader::ResultStatus::Success) return result;
    u64 title = 0;
    const auto title_status = secondary->ReadProgramId(title);
    if (title_status != Loader::ResultStatus::Success) return {title_status, {}};
    // This is the explicit top-level ownership seam. The existing global NCA,
    // NSP, NSO and deconstructed loaders and their registration are untouched.
    const bool registered = registration.Commit(process.GetProcessId(), title,
        [&](u64 process_id, u64 program_id) {
            auto factory = std::make_shared<FileSys::RomFSFactory>(
                *this, system.GetContentProvider(), system.GetFileSystemController());
            return system.GetFileSystemController().RegisterProcess(
                process_id, program_id, std::move(factory)).IsSuccess();
        });
    if (!registered) return {Loader::ResultStatus::ErrorNotInitialized, {}};
    is_loaded = true;
    return result;
}

Loader::ResultStatus DirectLoader::ReadProgramId(u64& output) {
    return secondary ? secondary->ReadProgramId(output) : preflight.status;
}
Loader::ResultStatus DirectLoader::ReadProgramIds(std::vector<u64>& output) {
    u64 title = 0;
    const auto status = ReadProgramId(title);
    if (status == Loader::ResultStatus::Success) output = {title};
    return status;
}
Loader::ResultStatus DirectLoader::ReadTitle(std::string& output) {
    return secondary ? secondary->ReadTitle(output) : preflight.status;
}
Loader::ResultStatus DirectLoader::ReadIcon(std::vector<u8>& output) {
    return secondary ? secondary->ReadIcon(output) : preflight.status;
}
Loader::ResultStatus DirectLoader::ReadControlData(FileSys::NACP& output) {
    return secondary ? secondary->ReadControlData(output) : preflight.status;
}
Loader::ResultStatus DirectLoader::ReadNSOModules(Modules& output) {
    return secondary ? secondary->ReadNSOModules(output) : preflight.status;
}
Loader::ResultStatus DirectLoader::ReadRomFS(FileSys::VirtualFile& output) {
    if (preflight.status != Loader::ResultStatus::Success) return preflight.status;
    return romfs.Read(output);
}

std::unique_ptr<DirectLoader> CreateDirectDevelopmentLoader(Inputs inputs) {
    return std::make_unique<DirectLoader>(std::move(inputs));
}
} // namespace Prospero::Development
