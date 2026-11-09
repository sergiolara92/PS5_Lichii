// SPDX-License-Identifier: GPL-3.0-or-later
#include "development_input.h"

#include <array>
#include <algorithm>
#include <utility>
#include <chrono>
#include <cstdio>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include "core/file_sys/romfs.h"

namespace Prospero::Development {
namespace {
constexpr std::array<const char*, 13> kModules{
    "rtld", "main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4",
    "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9", "sdk"};

class ExplicitParentFile final : public FileSys::VfsFile {
public:
    ExplicitParentFile(FileSys::VirtualFile source_, FileSys::VirtualDir parent_, std::shared_ptr<void> owner_ = {})
        : owner(std::move(owner_)), source(std::move(source_)), parent(std::move(parent_)) {}
    std::string GetName() const override { return source->GetName(); }
    std::size_t GetSize() const override { return source->GetSize(); }
    bool Resize(std::size_t) override { return false; }
    FileSys::VirtualDir GetContainingDirectory() const override { return parent; }
    bool IsWritable() const override { return false; }
    bool IsReadable() const override { return source->IsReadable(); }
    std::size_t Read(u8* output, std::size_t length, std::size_t offset) const override {
        const auto size = source->GetSize();
        if (offset >= size) return 0;
        return source->Read(output, std::min(length, size - offset), offset);
    }
    std::size_t Write(const u8*, std::size_t, std::size_t) override { return 0; }
    bool Rename(std::string_view) override { return false; }
private:
    // RealVfsFile's destructor calls its filesystem: keep that owner alive until
    // both child handles have been destroyed (members tear down in reverse).
    std::shared_ptr<void> owner;
    FileSys::VirtualFile source;
    FileSys::VirtualDir parent;
};
} // namespace

InputReport ValidateInputs(const Inputs& inputs) {
    InputReport report;
    const auto fail = [&](Loader::ResultStatus status, std::string detail) {
        report.status = status; report.detail = std::move(detail); return report;
    };
    if (!inputs.exefs) return fail(Loader::ResultStatus::ErrorNullFile, "Explicit ExeFS root is missing");
    if (inputs.romfs_directory && inputs.romfs_image)
        return fail(Loader::ResultStatus::ErrorNotImplemented, "Choose a RomFS directory or image, not both");
    if (!inputs.exefs->GetFile("main")) return fail(Loader::ResultStatus::ErrorNullFile, "ExeFS/main is missing");
    const auto npdm = inputs.exefs->GetFile("main.npdm");
    if (!npdm) return fail(Loader::ResultStatus::ErrorMissingNPDM, "ExeFS/main.npdm is missing");
    std::array<u8, 4> magic{};
    if (npdm->GetSize() < 0x80 || npdm->Read(magic.data(), magic.size(), 0) != magic.size() ||
        magic != std::array<u8, 4>{'M', 'E', 'T', 'A'})
        return fail(Loader::ResultStatus::ErrorBadNPDMHeader, "NPDM header is truncated or has wrong magic");
    for (const auto& required : inputs.required_modules) {
        if (std::find(kModules.begin(), kModules.end(), required) == kModules.end())
            return fail(Loader::ResultStatus::ErrorLoadingNSO, "Unknown required module: " + required);
        if (!inputs.exefs->GetFile(required))
            return fail(Loader::ResultStatus::ErrorLoadingNSO, "Required module is missing: " + required);
    }
    for (const auto* name : kModules) {
        const auto module = inputs.exefs->GetFile(name);
        if (!module) continue;
        if (module->GetSize() < 0x100 || module->Read(magic.data(), magic.size(), 0) != magic.size() ||
            magic != std::array<u8, 4>{'N', 'S', 'O', '0'})
            return fail(Loader::ResultStatus::ErrorLoadingNSO, std::string("Invalid NSO header: ") + name);
        report.modules.emplace_back(name);
    }
    report.status = Loader::ResultStatus::Success;
    report.detail = "Explicit deconstructed development input; structural preflight only";
    return report;
}

FileSys::VirtualFile MainWithExplicitParent(const Inputs& inputs) {
    if (!inputs.exefs) return {};
    const auto main = inputs.exefs->GetFile("main");
    return main ? std::make_shared<ExplicitParentFile>(main, inputs.exefs, inputs.lifetime_owner) : nullptr;
}

LazyRomFs::LazyRomFs(const Inputs& inputs)
    : owner(inputs.lifetime_owner), directory(inputs.romfs_directory), image(inputs.romfs_image) {}

Loader::ResultStatus LazyRomFs::Read(FileSys::VirtualFile& output) {
    std::call_once(built, [&] {
        if (!image && directory) {
            const auto start = std::chrono::steady_clock::now();
#if defined(__GLIBC__)
            const auto before = mallinfo2();
#endif
            image = FileSys::CreateRomFS(directory, nullptr);
            std::fprintf(stderr, "EDEN_ROMFS built from a folder: %llu bytes in %lld ms\n",
                         static_cast<unsigned long long>(image ? image->GetSize() : 0),
                         static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start).count()));
#if defined(__GLIBC__)
            const auto after = mallinfo2();
            std::fprintf(stderr, "EDEN_ROMFS heap: %zu bytes before, %zu after\n",
                         before.uordblks + before.hblkhd, after.uordblks + after.hblkhd);
#endif
        }
        if (image && owner) image = std::make_shared<ExplicitParentFile>(image, image->GetContainingDirectory(), owner);
    });
    output = image;
    return image ? Loader::ResultStatus::Success : Loader::ResultStatus::ErrorNoRomFS;
}

bool RegistrationGate::Commit(u64 process_id, u64 title_id,
                              const std::function<bool(u64, u64)>& register_owner) {
    if (registered || !register_owner || !register_owner(process_id, title_id)) return false;
    registered = true;
    return true;
}
} // namespace Prospero::Development
