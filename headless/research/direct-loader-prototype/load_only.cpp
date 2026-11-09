// SPDX-License-Identifier: GPL-3.0-or-later
// Prepared actual-core host target. Do not confuse mapping with guest execution.
#include "direct_development_loader.h"
#include <cstdio>
#include <filesystem>
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/filesystem/romfs_controller.h"
#include "core/hle/service/os/process.h"

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fputs("Usage: prospero_direct_load_only EXEFS_DIRECTORY [LOOSE_ROMFS_DIRECTORY]\n", stderr);
        return 2;
    }
    // Run from a fresh research directory. This target never installs anything
    // or supplies keys/firmware. The caller controls isolated user-data paths.
    if (!std::filesystem::is_directory("user")) {
        std::fputs("Create an isolated working directory containing user/ before this host probe.\n", stderr);
        return 2;
    }
    if (!std::filesystem::is_directory(argv[1]) ||
        (argc == 3 && !std::filesystem::is_directory(argv[2]))) {
        std::fputs("An explicitly supplied ExeFS/RomFS directory is missing.\n", stderr);
        return 2;
    }
    const auto isolated_user = std::filesystem::absolute("user").lexically_normal();
    if (Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir).lexically_normal() != isolated_user) {
        std::fputs("Eden did not select this working directory's portable user/ tree; refusing probe.\n", stderr);
        return 2;
    }
    Common::FS::CreateEdenPaths();
    Common::Log::Initialize();
    Common::Log::Filter filter;
    filter.ParseFilterString("*:Debug");
    Common::Log::SetGlobalFilter(filter);
    Common::Log::Start();
    Settings::values.renderer_backend = Settings::RendererBackend::Null;
    Settings::values.memory_layout_mode = Settings::MemoryLayout::Memory_4Gb;
    Settings::values.use_multi_core = false;
    Core::System system;
    system.Initialize();
    system.Kernel().Initialize();
    int result_code = 1;
    {
        auto vfs = system.GetFilesystem();
        Prospero::Development::Inputs inputs;
        inputs.exefs = vfs->OpenDirectory(argv[1], FileSys::OpenMode::Read);
        if (argc == 3) inputs.romfs_directory = vfs->OpenDirectory(argv[2], FileSys::OpenMode::Read);
        inputs.required_modules = {"rtld", "main", "subsdk0", "sdk"};
        auto loader = Prospero::Development::CreateDirectDevelopmentLoader(std::move(inputs));
        const auto& preflight = loader->Preflight();
        std::printf("DIRECT_LOAD_ONLY preflight=%u modules=%zu guest_execution=not_started detail=%s\n",
                    static_cast<unsigned>(preflight.status), preflight.modules.size(), preflight.detail.c_str());
        if (preflight.status == Loader::ResultStatus::Success) {
            system.GetFileSystemController().CreateFactories(*vfs);
            Service::Process process(system);
            Loader::ResultStatus status = Loader::ResultStatus::ErrorNotInitialized;
            const bool mapped = process.Initialize(*loader, status);
            Loader::AppLoader::Modules modules;
            if (mapped) loader->ReadNSOModules(modules);
            std::printf("DIRECT_LOAD_ONLY mapped=%u loader_status=%u filesystem_registered=%u module_count=%zu "
                        "process_id=%llu program_id=%016llx guest_execution=not_started\n",
                        mapped, static_cast<unsigned>(status), loader->FilesystemRegistered(), modules.size(),
                        static_cast<unsigned long long>(process.GetProcessId()),
                        static_cast<unsigned long long>(process.GetProgramId()));
            for (const auto& [address, name] : modules)
                std::printf("DIRECT_LOAD_ONLY module=%s address=%016llx\n", name.c_str(),
                            static_cast<unsigned long long>(address));
            if (mapped) {
                Service::FileSystem::ProgramId registered_title{};
                std::shared_ptr<Service::FileSystem::SaveDataController> save;
                std::shared_ptr<Service::FileSystem::RomFsController> romfs;
                const auto opened = system.GetFileSystemController().OpenProcess(
                    &registered_title, &save, &romfs, process.GetProcessId());
                const auto image = opened.IsSuccess() && romfs ? romfs->OpenRomFSCurrentProcess() : nullptr;
                std::printf("DIRECT_LOAD_ONLY factory_open_result=%u registered_title=%016llx romfs_size=%llu\n",
                            opened.raw, static_cast<unsigned long long>(registered_title),
                            image ? static_cast<unsigned long long>(image->GetSize()) : 0ULL);
            }
            // Intentionally no process.Run(), system.Run() or System::Load().
            result_code = mapped ? 0 : 1;
        }
    }
    system.Kernel().Shutdown();
    Common::Log::Stop();
    return result_code;
}
