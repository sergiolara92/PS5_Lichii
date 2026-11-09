// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_read_error.h"

namespace FileSys::ZipVfs {
class ZipError : public FileSys::VfsReadError {
public:
    using FileSys::VfsReadError::VfsReadError;
};
struct Options {
    // Existing parent directory. A fresh owned subdirectory is created lazily
    // on the first nonempty read; entry paths are never used as disk paths.
    std::filesystem::path cache_directory;
    std::uint64_t cache_quota_bytes = 512ULL * 1024 * 1024;
    std::uint64_t max_entry_bytes = 512ULL * 1024 * 1024;
    bool exclude_listing = false;
};
struct Stats {
    std::uint64_t files{}, directories{}, total_uncompressed_bytes{};
    std::uint64_t cache_bytes{}, extractions{}, evictions{}, archive_bytes_read{};
    std::uint64_t excluded_listing_files{};
};
class Archive {
public:
    static std::shared_ptr<Archive> Open(const std::filesystem::path& archive, Options options);
    ~Archive();
    VirtualDir Root() const;
    VirtualDir ExeFs() const; // explicit base/exefs
    VirtualDir RomFs() const; // explicit base/romfs
    Stats GetStats() const;
    struct State;
private:
    explicit Archive(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};
// All mutations are rejected. Nonempty Read failures throw ZipError, rather
// than returning fabricated bytes or treating corrupt input as successful EOF.
// EOF reads return 0. Each first read verifies the entire entry's CRC before
// exposing any bytes. Reads are serialized; open VFS objects retain the state.
} // namespace FileSys::ZipVfs
