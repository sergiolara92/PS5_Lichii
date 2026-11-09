// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Prospero::Development {
enum class DescriptorState { Absent, Disabled, Ready, Invalid };
enum class ResearchMode { LoadOnly, Execute };
enum class ResearchRomFs { None, Loose, Image, Zip };
struct ResearchDescriptor {
    DescriptorState state = DescriptorState::Disabled;
    ResearchMode mode = ResearchMode::LoadOnly;
    ResearchRomFs romfs_kind = ResearchRomFs::None;
    std::filesystem::path exefs, romfs_path, cache_directory;
    // A game card image: ExeFS and RomFS come from its program NCA (no exefs/romfs fields).
    std::filesystem::path xci;
    // A NUP container: ExeFS from its newest update (else its base), RomFS the update over the base.
    std::filesystem::path nup;
    bool nup_update = true; // false: the base package alone, its updates left out
    // A submission package (NSP): ExeFS and RomFS from its program NCA, as a game card image.
    std::filesystem::path nsp;
    std::uint64_t cache_quota_bytes = 1024ULL * 1024 * 1024;
    std::uint64_t max_entry_bytes = 512ULL * 1024 * 1024;
    bool exclude_listing = false;
    bool pauth_disabled_profile = false;
    bool remote_region_profile = false;
    bool strict_memory_aborts_profile = false;
    std::vector<std::string> required_modules{"main"};
    std::string error;
};
ResearchDescriptor ParseResearchDescriptor(const std::string& text);
ResearchDescriptor ReadResearchDescriptor(const std::filesystem::path& file);
} // namespace Prospero::Development
