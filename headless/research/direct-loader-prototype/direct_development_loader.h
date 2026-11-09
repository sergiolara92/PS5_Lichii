// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include "development_input.h"
#include "core/loader/deconstructed_rom_directory.h"

namespace Prospero::Development {

// Constructed explicitly by research tools only. It is never added to GetLoader
// and is never used as an NCA/NSP's internal secondary loader.
class DirectLoader final : public Loader::AppLoader {
public:
    explicit DirectLoader(Inputs inputs);
    Loader::FileType GetFileType() const override { return Loader::FileType::DeconstructedRomDirectory; }
    LoadResult Load(Kernel::KProcess& process, Core::System& system) override;
    Loader::ResultStatus ReadProgramId(u64& output) override;
    Loader::ResultStatus ReadProgramIds(std::vector<u64>& output) override;
    Loader::ResultStatus ReadTitle(std::string& output) override;
    Loader::ResultStatus ReadIcon(std::vector<u8>& output) override;
    Loader::ResultStatus ReadControlData(FileSys::NACP& output) override;
    Loader::ResultStatus ReadNSOModules(Modules& output) override;
    Loader::ResultStatus ReadRomFS(FileSys::VirtualFile& output) override;
    bool IsRomFSUpdatable() const override { return false; }
    const InputReport& Preflight() const { return preflight; }
    bool FilesystemRegistered() const { return registration.Registered(); }
private:
    Inputs inputs;
    InputReport preflight;
    LazyRomFs romfs;
    RegistrationGate registration;
    bool attempted_load = false; // Mapping can leave partial kernel state: never retry it in place.
    std::unique_ptr<Loader::AppLoader_DeconstructedRomDirectory> secondary;
};

std::unique_ptr<DirectLoader> CreateDirectDevelopmentLoader(Inputs inputs);
} // namespace Prospero::Development
