// SPDX-License-Identifier: GPL-3.0-or-later
#include "research_frontend.h"
#include "research_execution_profile.h"
#include "research_npdm_snapshot.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/vfs/vfs_layered.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "direct_development_loader.h"
#include "factory_acquisition_gate.h"
#include "core/loader/development_factory.h"
#include "zip_vfs.h"
#include <atomic>
#include <cstdio>
#include <utility>
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/os/process.h"

namespace Prospero::Development {
namespace {
struct BackingResources {
    std::shared_ptr<FileSys::RealVfsFilesystem> vfs;
    std::shared_ptr<FileSys::ZipVfs::Archive> archive;
    std::shared_ptr<FileSys::XCI> xci;
    std::shared_ptr<FileSys::NupContainer> nup;
    std::shared_ptr<FileSys::NSP> nsp;
};
// Eden's ExeFS loader registers the process too, with the first .romfs file beside main: give it
// the image, so the game finds its RomFS whichever registration stays (an image has no folder).
void RomFsBesideMain(Inputs& inputs) {
    const auto exefs = inputs.exefs;
    const auto beside = std::make_shared<FileSys::OffsetVfsFile>(
        inputs.romfs_image, inputs.romfs_image->GetSize(), 0, "game.romfs");
    inputs.exefs = FileSys::LayeredVfsDirectory::MakeLayeredDirectory(
        {exefs, std::make_shared<FileSys::VectorVfsDirectory>(
                    std::vector<FileSys::VirtualFile>{beside}, std::vector<FileSys::VirtualDir>{},
                    exefs->GetName(), exefs->GetParentDirectory())},
        exefs->GetName());
}
} // namespace
struct ResearchRoute::State {
    ResearchDescriptor descriptor;
    std::shared_ptr<BackingResources> backing;
    Inputs inputs;
    std::filesystem::path entry;
    bool consumed = false;
    bool prepared = false;
    bool strict_target_verified = false;
    u64 strict_program_id = 0;
    std::atomic<unsigned> matches{};
};

FactoryLease::~FactoryLease() { if (token) Loader::RemoveDevelopmentFactory(token); }
FactoryLease::FactoryLease(FactoryLease&& other) noexcept : token(std::exchange(other.token, 0)) {}
FactoryLease& FactoryLease::operator=(FactoryLease&& other) noexcept {
    if (this != &other) {
        if (token) Loader::RemoveDevelopmentFactory(token);
        token = std::exchange(other.token, 0);
    }
    return *this;
}
ResearchRoute::ResearchRoute(const std::filesystem::path& file) : state(std::make_shared<State>()) {
    state->descriptor = ReadResearchDescriptor(file);
}
bool ResearchRoute::Pending() const { return state->descriptor.state == DescriptorState::Ready && !state->consumed; }
bool ResearchRoute::Invalid() const { return state->descriptor.state == DescriptorState::Invalid; }
const std::string& ResearchRoute::Error() const { return state->descriptor.error; }
ResearchMode ResearchRoute::Mode() const { return state->descriptor.mode; }
bool ResearchRoute::PAuthDisabledProfile() const { return state->descriptor.pauth_disabled_profile; }
bool ResearchRoute::RemoteRegionProfile() const { return state->descriptor.remote_region_profile; }
// Only the actual bounded NPDM snapshot verified by TakeEntryPoint can select
// this research layout. This does not attest a retail RAM configuration.
bool ResearchRoute::VerifiedExtendedMemoryProfile() const {
    return state->prepared && state->descriptor.mode == ResearchMode::Execute &&
           state->descriptor.remote_region_profile && state->strict_target_verified &&
           (state->strict_program_id >> 56) == 0x04;
}
bool ResearchRoute::StrictMemoryAbortsRequested() const { return state->descriptor.strict_memory_aborts_profile; }

std::string ResearchRoute::TakeEntryPoint() {
    if (!Pending()) throw std::runtime_error("No pending explicit research input");
    state->consumed = true;
    try {
        state->backing = std::make_shared<BackingResources>();
        state->backing->vfs = std::make_shared<FileSys::RealVfsFilesystem>();
        auto& vfs = *state->backing->vfs;
        const auto& descriptor = state->descriptor;
        if (!descriptor.xci.empty()) {
            // Decrypted through the console's keys as read: no folder RomFS to build.
            const auto file = vfs.OpenFile(descriptor.xci.string(), FileSys::OpenMode::Read);
            if (!file) throw std::runtime_error("Explicit XCI cannot be opened");
            state->backing->xci = std::make_shared<FileSys::XCI>(file);
            const auto& xci = *state->backing->xci;
            if (xci.GetStatus() != Loader::ResultStatus::Success || xci.GetProgramNCAStatus() != Loader::ResultStatus::Success)
                throw std::runtime_error("Explicit XCI: " + Loader::GetResultStatusString(xci.GetProgramNCAStatus()));
            const auto program = xci.GetNCAByType(FileSys::NCAContentType::Program);
            state->inputs.exefs = program ? program->GetExeFS() : nullptr;
            state->inputs.romfs_image = program ? program->GetRomFS() : nullptr;
            if (!state->inputs.exefs || !state->inputs.romfs_image)
                throw std::runtime_error("Explicit XCI has no program ExeFS and RomFS");
            std::fprintf(stderr, "EDEN_ROMFS from a game card image: %llu bytes\n",
                         static_cast<unsigned long long>(state->inputs.romfs_image->GetSize()));
            RomFsBesideMain(state->inputs);
        } else if (!descriptor.nsp.empty()) {
            // As a game card image: the program NCA's ExeFS and RomFS, decrypted through the console's
            // keys (and the package's own tickets) as read.
            const auto file = vfs.OpenFile(descriptor.nsp.string(), FileSys::OpenMode::Read);
            if (!file) throw std::runtime_error("Explicit NSP cannot be opened");
            state->backing->nsp = std::make_shared<FileSys::NSP>(file);
            const auto& nsp = *state->backing->nsp;
            if (nsp.GetStatus() != Loader::ResultStatus::Success || nsp.GetProgramStatus() != Loader::ResultStatus::Success)
                throw std::runtime_error("Explicit NSP: " + Loader::GetResultStatusString(
                    nsp.GetStatus() != Loader::ResultStatus::Success ? nsp.GetStatus() : nsp.GetProgramStatus()));
            state->inputs.exefs = nsp.GetExeFS();
            state->inputs.romfs_image = nsp.GetRomFS();
            if (!state->inputs.exefs || !state->inputs.romfs_image)
                throw std::runtime_error("Explicit NSP has no program ExeFS and RomFS");
            std::fprintf(stderr, "EDEN_ROMFS from a submission package: %llu bytes\n",
                         static_cast<unsigned long long>(state->inputs.romfs_image->GetSize()));
            RomFsBesideMain(state->inputs);
        } else if (!descriptor.nup.empty()) {
            // One container file: every member is a view into it (a loose RomFS of tens of thousands
            // of files would each take a file of their own). As PokeDinner's NUP loader: the ExeFS
            // of the newest update (else the base), the update's RomFS over the base's; a
            // standalone update carries the whole tree.
            const auto file = vfs.OpenFile(descriptor.nup.string(), FileSys::OpenMode::Read);
            if (!file) throw std::runtime_error("Explicit NUP cannot be opened");
            state->backing->nup = std::make_shared<FileSys::NupContainer>(file);
            auto& nup = *state->backing->nup;
            if (nup.GetStatus() != Loader::ResultStatus::Success)
                throw std::runtime_error("Explicit NUP: " + Loader::GetResultStatusString(nup.GetStatus()));
            const auto base = nup.FindBasePackage();
            const auto update = descriptor.nup_update || !nup.FindBasePackage() ? nup.FindBestUpdatePackage()
                                                                                : std::optional<std::size_t>{};
            const bool base_runs = base && nup.GetExeFSDir(*base) != nullptr;
            const bool update_runs = update && nup.GetExeFSDir(*update) != nullptr;
            if (!base_runs && !update_runs) throw std::runtime_error("Explicit NUP has no package with an ExeFS");
            state->inputs.exefs = nup.GetExeFSDir(update_runs ? *update : *base);
            const auto romfs = FileSys::NupContainer::MergeShadowDirs(
                base ? nup.GetRomFSDir(*base) : FileSys::VirtualDir{},
                update ? nup.GetRomFSDir(*update) : FileSys::VirtualDir{});
            state->inputs.romfs_image = romfs ? FileSys::CreateRomFS(romfs) : nullptr;
            if (!state->inputs.romfs_image) throw std::runtime_error("Explicit NUP has no RomFS");
            RomFsBesideMain(state->inputs);
            std::fprintf(stderr, "EDEN_ROMFS from a NUP: %zu members, base %s, update %s\n", nup.GetFileCount(),
                         base ? "yes" : "no",
                         update ? nup.GetPackages()[*update].display_version.c_str() : "no");
        } else {
        if (vfs.GetEntryType(descriptor.exefs.string()) != FileSys::VfsEntryType::Directory)
            throw std::runtime_error("Explicit ExeFS path is not an accessible directory");
        state->inputs.exefs = vfs.OpenDirectory(descriptor.exefs.string(), FileSys::OpenMode::Read);
        }
        state->inputs.required_modules = descriptor.required_modules;
        state->inputs.lifetime_owner = state->backing;
        if (descriptor.romfs_kind == ResearchRomFs::Loose) {
            if (vfs.GetEntryType(descriptor.romfs_path.string()) != FileSys::VfsEntryType::Directory)
                throw std::runtime_error("Explicit loose RomFS path is not an accessible directory");
            state->inputs.romfs_directory = vfs.OpenDirectory(descriptor.romfs_path.string(), FileSys::OpenMode::Read);
            if (!state->inputs.romfs_directory) throw std::runtime_error("Explicit loose RomFS directory cannot be opened");
        } else if (descriptor.romfs_kind == ResearchRomFs::Image) {
            state->inputs.romfs_image = vfs.OpenFile(descriptor.romfs_path.string(), FileSys::OpenMode::Read);
            if (!state->inputs.romfs_image) throw std::runtime_error("Explicit RomFS image cannot be opened");
            RomFsBesideMain(state->inputs);
        } else if (descriptor.romfs_kind == ResearchRomFs::Zip) {
            FileSys::ZipVfs::Options options;
            options.cache_directory = descriptor.cache_directory;
            options.cache_quota_bytes = descriptor.cache_quota_bytes;
            options.max_entry_bytes = descriptor.max_entry_bytes;
            options.exclude_listing = descriptor.exclude_listing;
            state->backing->archive = FileSys::ZipVfs::Archive::Open(descriptor.romfs_path, options);
            state->inputs.romfs_directory = state->backing->archive->RomFs();
            if (!state->inputs.romfs_directory) throw std::runtime_error("ZIP has no explicit base/romfs root");
        }
        const auto report = ValidateInputs(state->inputs);
        if (report.status != Loader::ResultStatus::Success) throw std::runtime_error("Research preflight: " + report.detail);
        if (descriptor.strict_memory_aborts_profile && Mode() == ResearchMode::Execute) {
            const auto original = state->inputs.exefs;
            const auto npdm = original->GetFile("main.npdm");
            constexpr std::size_t MaximumNpdmBytes = 64 * 1024;
            const auto size = npdm ? npdm->GetSize() : 0;
            if (!npdm || size < 0x80 || size > MaximumNpdmBytes)
                throw std::runtime_error("Strict research NPDM snapshot requires a bounded complete metadata file");
            std::vector<u8> bytes(size);
            if (npdm->Read(bytes.data(), bytes.size(), 0) != size)
                throw std::runtime_error("Short strict research NPDM snapshot read");
            auto snapshot = std::make_shared<ImmutableNpdmFile>(std::move(bytes));
            FileSys::ProgramMetadata metadata;
            if (metadata.Load(snapshot) != Loader::ResultStatus::Success)
                throw std::runtime_error("Actual strict research NPDM parser rejected the input");
            state->strict_program_id = metadata.GetTitleID();
            state->strict_target_verified = SelectStrictMemoryProfile(
                true, true, metadata.GetTitleID(), metadata.Is64BitProgram(),
                metadata.GetAddressSpaceType() == FileSys::ProgramAddressSpaceType::Is39Bit,
                metadata.GetPoolPartition() == FileSys::PoolPartition::Application);
            auto metadata_directory = std::make_shared<FileSys::VectorVfsDirectory>(
                std::vector<FileSys::VirtualFile>{snapshot}, std::vector<FileSys::VirtualDir>{},
                original->GetName(), original->GetParentDirectory());
            state->inputs.exefs = FileSys::LayeredVfsDirectory::MakeLayeredDirectory(
                {metadata_directory, original}, original->GetName());
        }
        state->entry = (!descriptor.xci.empty() ? descriptor.xci
                        : !descriptor.nup.empty() ? descriptor.nup
                        : !descriptor.nsp.empty() ? descriptor.nsp : descriptor.exefs / "main").lexically_normal();
        state->prepared = true;
        std::fprintf(stderr, "PROSPERO_RESEARCH stage=prepared mode=%s modules=%zu guest_execution=not_started\n",
                     Mode() == ResearchMode::LoadOnly ? "load-only" : "execute", report.modules.size());
        return state->entry.string();
    } catch (const FileSys::ZipVfs::ZipError& error) {
        std::fprintf(stderr, "PROSPERO_RESEARCH stage=zip_error detail=%s\n", error.what());
        throw std::runtime_error(std::string("Research ZIP error: ") + error.what());
    }
}

FactoryLease ResearchRoute::InstallFactory(Core::System& expected_system, StrictMemoryAbortLease* strict_memory) const {
    if (!state->prepared) throw std::runtime_error("Research input was not prepared");
    const auto owned = state;
    const auto acquisition = std::make_shared<FactoryAcquisitionGate>(&expected_system);
    return FactoryLease(Loader::InstallDevelopmentFactory(
        [owned, acquisition, strict_memory](Core::System& system, FileSys::VirtualFile file, u64 program_id, size_t program_index)
            -> std::unique_ptr<Loader::AppLoader> {
            const bool exact = file && (file->GetName() == "main" || !owned->descriptor.xci.empty() ||
                                        !owned->descriptor.nup.empty() || !owned->descriptor.nsp.empty()) &&
                std::filesystem::path(file->GetFullPath()).lexically_normal() == owned->entry;
            if (acquisition->Acquire(&system, exact, program_id == 0 && program_index == 0) ==
                FactoryAcquisitionGate::Decision::Ignore) return {};
            ++owned->matches;
            std::fprintf(stderr, "PROSPERO_RESEARCH stage=exact_factory_match guest_execution=not_started\n");
            auto loader = CreateDirectDevelopmentLoader(owned->inputs);
            if (owned->strict_target_verified) {
                u64 actual_program_id = 0;
                if (!loader || loader->ReadProgramId(actual_program_id) != Loader::ResultStatus::Success ||
                    actual_program_id != owned->strict_program_id)
                    throw std::runtime_error("Strict research loader metadata identity mismatch");
                if (!strict_memory)
                    throw std::runtime_error("Strict execution requires its scoped CPU setting lease");
                // The exact immutable metadata has been parsed by the real loader;
                // no process/JIT exists yet. This lease outlives Core teardown.
                strict_memory->Activate();
                std::fprintf(stderr, "NATIVE_RESEARCH strict_memory_aborts=1 ignore_memory_aborts=0 program_id=%016llx\n",
                             static_cast<unsigned long long>(actual_program_id));
            }
            return loader;
        }));
}

void ResearchRoute::CheckExecuteEnabled() const {
#ifndef PROSPERO_DIRECT_ALLOW_EXECUTE
    throw std::runtime_error("Research execute route is disabled in this build; validate host/Core linking and use load-only first");
#else
    std::fprintf(stderr, "PROSPERO_RESEARCH stage=execute_requested compatibility=unvalidated\n");
#endif
}

ResearchLoadResult ResearchRoute::PerformLoadOnly() const {
    ResearchLoadResult report;
    try {
        if (!state->prepared) throw std::runtime_error("Research input was not prepared");
        const bool multicore = Settings::values.use_multi_core.GetValue();
        Settings::values.use_multi_core = false;
        SCOPE_EXIT { Settings::values.use_multi_core = multicore; };
        Core::System system;
        system.Initialize();
        system.Kernel().Initialize();
        SCOPE_EXIT { system.Kernel().Shutdown(); };
        system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
        auto lease = InstallFactory(system);
        state->matches = 0;
        auto file = system.GetFilesystem()->OpenFile(state->entry.string(), FileSys::OpenMode::Read);
        auto loader = Loader::GetLoader(system, file, 0, 0);
        if (!loader || state->matches != 1)
            throw std::runtime_error("Explicit development factory did not match; refusing ordinary loader fallback");
        Service::Process process(system);
        report.mapped = process.Initialize(*loader, report.status);
        Loader::AppLoader::Modules modules;
        if (report.mapped) loader->ReadNSOModules(modules);
        report.modules = modules.size();
        report.detail = Loader::GetResultStatusString(report.status);
        std::fprintf(stderr, "PROSPERO_RESEARCH stage=%s loader_status=%u modules=%zu guest_execution=not_started\n",
                     report.mapped ? "modules_mapped" : "map_failed", static_cast<unsigned>(report.status), report.modules);
        // Deliberately never call Process::Run, System::Run or System::Load.
    } catch (const FileSys::ZipVfs::ZipError& error) {
        report.mapped = false;
        report.detail = std::string("Research ZIP read error: ") + error.what();
        std::fprintf(stderr, "PROSPERO_RESEARCH stage=zip_error detail=%s\n", error.what());
    } catch (const std::exception& error) {
        report.mapped = false;
        report.detail = error.what();
        std::fprintf(stderr, "PROSPERO_RESEARCH stage=load_only_error detail=%s\n", error.what());
    }
    return report;
}
} // namespace Prospero::Development
