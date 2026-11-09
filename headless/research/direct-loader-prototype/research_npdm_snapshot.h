// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstring>
#include <vector>
#include <utility>
#include "core/file_sys/vfs/vfs.h"
namespace Prospero::Development {
// Stable identical NPDM bytes for all real loader reloads in a strict execution.
// The source file is never edited and NSO/RomFS data remains in the real VFS.
class ImmutableNpdmFile final : public FileSys::VfsFile {
public:
    explicit ImmutableNpdmFile(std::vector<u8> bytes) : bytes(std::move(bytes)) {}
    std::string GetName() const override { return "main.npdm"; }
    std::size_t GetSize() const override { return bytes.size(); }
    bool Resize(std::size_t) override { return false; }
    FileSys::VirtualDir GetContainingDirectory() const override { return {}; }
    bool IsWritable() const override { return false; }
    bool IsReadable() const override { return true; }
    std::size_t Read(u8* output, std::size_t length, std::size_t offset) const override {
        if (length == 0 || offset >= bytes.size()) return 0;
        const auto count = std::min(length, bytes.size() - offset);
        std::memcpy(output, bytes.data() + offset, count);
        return count;
    }
    std::size_t Write(const u8*, std::size_t, std::size_t) override { return 0; }
    bool Rename(std::string_view) override { return false; }
private:
    const std::vector<u8> bytes;
};
} // namespace Prospero::Development
