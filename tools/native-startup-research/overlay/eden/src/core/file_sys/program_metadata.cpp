// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstddef>
#include <vector>

#include "common/logging.h"
#include "common/scope_exit.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/loader/loader.h"

namespace FileSys {

ProgramMetadata::ProgramMetadata() = default;

ProgramMetadata::~ProgramMetadata() = default;

Loader::ResultStatus ProgramMetadata::Load(VirtualFile file) {
    // Parser-only experiment: validate the declared containers and normalize
    // their common fields. This does not authenticate RSA signatures, and does
    // not change any caller's existing cryptographic verification policy.
    const auto contains = [](u64 size, u64 offset, u64 length) {
        return offset <= size && length <= size - offset;
    };
    const auto disjoint = [](u64 first, u64 first_size, u64 second, u64 second_size) {
        return first + first_size <= second || second + second_size <= first;
    };
    const auto sections_valid = [&](u64 size, u64 minimum,
                                    const std::array<std::pair<u64, u64>, 3>& sections) {
        for (const auto& [offset, length] : sections) {
            if (!contains(size, offset, length) || (length && offset < minimum)) return false;
        }
        for (size_t i = 0; i < sections.size(); ++i) {
            for (size_t j = i + 1; j < sections.size(); ++j) {
                if (sections[i].second && sections[j].second &&
                    !disjoint(sections[i].first, sections[i].second,
                              sections[j].first, sections[j].second)) return false;
            }
        }
        return true;
    };

    Header header{};
    AcidHeader acid{};
    AciHeader aci{};
    FileAccessControl fac{};
    FileAccessHeader fah{};
    if (!file || file->GetSize() < sizeof(Header) ||
        file->ReadObject(&header) != sizeof(Header) ||
        header.magic != std::array<char, 4>{'M', 'E', 'T', 'A'}) {
        return Loader::ResultStatus::ErrorBadNPDMHeader;
    }
    const u64 file_size = file->GetSize();
    const u64 acid_start = header.acid_offset;
    const u64 acid_size = header.acid_size;
    const u64 aci_start = header.aci_offset;
    const u64 aci_size = header.aci_size;
    if (acid_start < sizeof(Header) || !contains(file_size, acid_start, acid_size)) {
        return Loader::ResultStatus::ErrorBadACIDHeader;
    }
    if (aci_start < sizeof(Header) || aci_size < sizeof(AciHeader) ||
        !contains(file_size, aci_start, aci_size) ||
        !disjoint(acid_start, acid_size, aci_start, aci_size)) {
        return Loader::ResultStatus::ErrorBadACIHeader;
    }

    // These are documented wire layouts, not a scan for arbitrary magic bytes.
    // Reject ambiguous inputs rather than guessing between the two key sizes.
    u64 rsa_size = 0;
    for (const u64 candidate_rsa_size : {0x100ULL, 0x180ULL}) {
        const u64 prefix = candidate_rsa_size * 2;
        AcidHeader candidate{};
        if (!contains(acid_size, prefix, sizeof(AcidHeader))) continue;
        if (file->ReadObject(&candidate, acid_start + prefix) != sizeof(AcidHeader)) {
            return Loader::ResultStatus::ErrorBadACIDHeader;
        }
        if (candidate.magic != std::array<char, 4>{'A', 'C', 'I', 'D'}) continue;
        if (rsa_size != 0 || candidate.nca_size < candidate_rsa_size + sizeof(AcidHeader) ||
            !contains(acid_size, candidate_rsa_size, candidate.nca_size)) {
            return Loader::ResultStatus::ErrorBadACIDHeader;
        }
        rsa_size = candidate_rsa_size;
        acid = candidate;
    }
    if (!rsa_size) return Loader::ResultStatus::ErrorBadACIDHeader;
    const u64 signed_end = rsa_size + static_cast<u64>(acid.nca_size);
    const u64 common_end = rsa_size * 2 + sizeof(AcidHeader);
    if (acid.fac_size < 0x2c || !contains(signed_end, acid.fac_offset, acid.fac_size)) {
        return Loader::ResultStatus::ErrorBadFileAccessControl;
    }
    if (acid.kac_size % sizeof(u32) != 0 ||
        !contains(signed_end, acid.kac_offset, acid.kac_size)) {
        return Loader::ResultStatus::ErrorBadKernelCapabilityDescriptors;
    }
    if (!sections_valid(signed_end, common_end, {{{acid.fac_offset, acid.fac_size},
                                                {acid.sac_offset, acid.sac_size},
                                                {acid.kac_offset, acid.kac_size}}})) {
        return Loader::ResultStatus::ErrorBadACIDHeader;
    }
    if (file->ReadObject(&aci, aci_start) != sizeof(AciHeader) ||
        aci.magic != std::array<char, 4>{'A', 'C', 'I', '0'}) {
        return Loader::ResultStatus::ErrorBadACIHeader;
    }
    if (aci.fah_size < 0x1c || !contains(aci_size, aci.fah_offset, aci.fah_size)) {
        return Loader::ResultStatus::ErrorBadFileAccessHeader;
    }
    if (aci.kac_size % sizeof(u32) != 0 || !contains(aci_size, aci.kac_offset, aci.kac_size)) {
        return Loader::ResultStatus::ErrorBadKernelCapabilityDescriptors;
    }
    if (!sections_valid(aci_size, sizeof(AciHeader), {{{aci.fah_offset, aci.fah_size},
                                                     {aci.sac_offset, aci.sac_size},
                                                     {aci.kac_offset, aci.kac_size}}})) {
        return Loader::ResultStatus::ErrorBadACIHeader;
    }

    // FAC/FAH offsets are relative to their ACID/ACI starts, never the file start.
    const u64 fac_start = acid_start + static_cast<u64>(acid.fac_offset);
    const u64 fah_start = aci_start + static_cast<u64>(aci.fah_offset);
    if (file->ReadBytes(&fac.version, 1, fac_start) != 1 || fac.version == 0 ||
        file->ReadBytes(&fac.permissions, 8, fac_start + 4) != 8 ||
        file->ReadBytes(fac.unknown.data(), fac.unknown.size(), fac_start + 12) != fac.unknown.size()) {
        return Loader::ResultStatus::ErrorBadFileAccessControl;
    }
    if (file->ReadBytes(&fah.version, 1, fah_start) != 1 || fah.version == 0 ||
        file->ReadBytes(&fah.permissions, 8, fah_start + 4) != 8 ||
        file->ReadBytes(&fah.unk_offset, 4, fah_start + 12) != 4 ||
        file->ReadBytes(&fah.unk_size, 4, fah_start + 16) != 4 ||
        file->ReadBytes(&fah.unk_offset_2, 4, fah_start + 20) != 4 ||
        file->ReadBytes(&fah.unk_size_2, 4, fah_start + 24) != 4) {
        return Loader::ResultStatus::ErrorBadFileAccessHeader;
    }
    KernelCapabilityDescriptors capabilities(aci.kac_size / sizeof(u32));
    const u64 capability_start = aci_start + static_cast<u64>(aci.kac_offset);
    if (file->ReadBytes(capabilities.data(), aci.kac_size, capability_start) != aci.kac_size) {
        return Loader::ResultStatus::ErrorBadKernelCapabilityDescriptors;
    }

    // Failed loads leave the previous metadata intact.
    npdm_header = header;
    acid_header = acid;
    aci_header = aci;
    acid_file_access = fac;
    aci_file_access = fah;
    aci_kernel_capabilities = std::move(capabilities);
    return Loader::ResultStatus::Success;
}


Loader::ResultStatus ProgramMetadata::Reload(VirtualFile file) {
    const u64 original_program_id = aci_header.title_id;
    SCOPE_EXIT {
        aci_header.title_id = original_program_id;
    };

    return this->Load(file);
}

/*static*/ ProgramMetadata ProgramMetadata::GetDefault() {
    // Allow use of cores 0~3 and thread priorities 16~63.
    constexpr u32 default_thread_info_capability = 0x30043F7;

    ProgramMetadata result;

    result.LoadManual(
        true /*is_64_bit*/, FileSys::ProgramAddressSpaceType::Is39Bit /*address_space*/,
        0x2c /*main_thread_prio*/, 0 /*main_thread_core*/, 0x100000 /*main_thread_stack_size*/,
        0 /*title_id*/, 0xFFFFFFFFFFFFFFFF /*filesystem_permissions*/, 0 /*system_resource_size*/,
        {default_thread_info_capability} /*capabilities*/);

    return result;
}

void ProgramMetadata::LoadManual(bool is_64_bit, ProgramAddressSpaceType address_space,
                                 s32 main_thread_prio, u32 main_thread_core,
                                 u32 main_thread_stack_size, u64 title_id,
                                 u64 filesystem_permissions, u32 system_resource_size,
                                 KernelCapabilityDescriptors capabilities) {
    npdm_header.has_64_bit_instructions.Assign(is_64_bit);
    npdm_header.address_space_type.Assign(address_space);
    npdm_header.main_thread_priority = static_cast<u8>(main_thread_prio);
    npdm_header.main_thread_cpu = static_cast<u8>(main_thread_core);
    npdm_header.main_stack_size = main_thread_stack_size;
    aci_header.title_id = title_id;
    aci_file_access.permissions = filesystem_permissions;
    npdm_header.system_resource_size = system_resource_size;
    aci_kernel_capabilities = std::move(capabilities);
}

bool ProgramMetadata::Is64BitProgram() const {
    return bool(npdm_header.has_64_bit_instructions);
}

ProgramAddressSpaceType ProgramMetadata::GetAddressSpaceType() const {
    return npdm_header.address_space_type;
}

u8 ProgramMetadata::GetMainThreadPriority() const {
    return npdm_header.main_thread_priority;
}

u8 ProgramMetadata::GetMainThreadCore() const {
    return npdm_header.main_thread_cpu;
}

u32 ProgramMetadata::GetMainThreadStackSize() const {
    return npdm_header.main_stack_size;
}

u64 ProgramMetadata::GetTitleID() const {
    return aci_header.title_id;
}

u64 ProgramMetadata::GetFilesystemPermissions() const {
    return aci_file_access.permissions;
}

u32 ProgramMetadata::GetSystemResourceSize() const {
    return npdm_header.system_resource_size;
}

PoolPartition ProgramMetadata::GetPoolPartition() const {
    return acid_header.pool_partition;
}

const ProgramMetadata::KernelCapabilityDescriptors& ProgramMetadata::GetKernelCapabilities() const {
    return aci_kernel_capabilities;
}

void ProgramMetadata::Print() const {
    LOG_DEBUG(Service_FS, "Magic:                  {:.4}", npdm_header.magic.data());
    LOG_DEBUG(Service_FS, "Main thread priority:   {:#02x}", npdm_header.main_thread_priority);
    LOG_DEBUG(Service_FS, "Main thread core:       {}", npdm_header.main_thread_cpu);
    LOG_DEBUG(Service_FS, "Main thread stack size: {:#x} bytes", npdm_header.main_stack_size);
    LOG_DEBUG(Service_FS, "Process category:       {}", npdm_header.process_category);
    LOG_DEBUG(Service_FS, "Flags:                  {:#02x}", npdm_header.flags);
    LOG_DEBUG(Service_FS, " > 64-bit instructions: {}",
              npdm_header.has_64_bit_instructions ? "YES" : "NO");

    const char* address_space = "Unknown";
    switch (npdm_header.address_space_type) {
    case ProgramAddressSpaceType::Is36Bit:
        address_space = "64-bit (36-bit address space)";
        break;
    case ProgramAddressSpaceType::Is39Bit:
        address_space = "64-bit (39-bit address space)";
        break;
    case ProgramAddressSpaceType::Is32Bit:
        address_space = "32-bit";
        break;
    case ProgramAddressSpaceType::Is32BitNoMap:
        address_space = "32-bit (no map region)";
        break;
    }

    LOG_DEBUG(Service_FS, " > Address space:       {}\n", address_space);

    // Begin ACID printing (potential perms, signed)
    LOG_DEBUG(Service_FS, "Magic:                  {:.4}", acid_header.magic.data());
    LOG_DEBUG(Service_FS, "Flags:                  {:#02x}", acid_header.flags);
    LOG_DEBUG(Service_FS, " > Is Retail:           {}", acid_header.production_flag ? "YES" : "NO");
    LOG_DEBUG(Service_FS, "Title ID Min:           {:#016x}", acid_header.title_id_min);
    LOG_DEBUG(Service_FS, "Title ID Max:           {:#016x}", acid_header.title_id_max);
    LOG_DEBUG(Service_FS, "Filesystem Access:      {:#016x}\n", acid_file_access.permissions);

    // Begin ACI0 printing (actual perms, unsigned)
    LOG_DEBUG(Service_FS, "Magic:                  {:.4}", aci_header.magic.data());
    LOG_DEBUG(Service_FS, "Title ID:               {:#016x}", aci_header.title_id);
    LOG_DEBUG(Service_FS, "Filesystem Access:      {:#016x}\n", aci_file_access.permissions);
}
} // namespace FileSys
