// SPDX-License-Identifier: GPL-3.0-or-later
#include "zip_vfs.h"
#include "miniz.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace FileSys::ZipVfs {
namespace {
static_assert(sizeof(std::size_t) >= 8, "ZIP VFS requires a 64-bit Eden host");
using Path = std::filesystem::path;
int Seek(std::FILE* f, std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return -1;
#ifdef _WIN32
    return _fseeki64(f, static_cast<__int64>(offset), SEEK_SET);
#else
    static_assert(sizeof(off_t) >= 8, "64-bit off_t required");
    return fseeko(f, static_cast<off_t>(offset), SEEK_SET);
#endif
}
std::FILE* OpenRead(const Path& p) {
#ifdef _WIN32
    std::FILE* f = nullptr;
    return _wfopen_s(&f, p.c_str(), L"rb") == 0 ? f : nullptr;
#else
    return std::fopen(p.c_str(), "rb");
#endif
}
std::FILE* OpenExclusive(const Path& p, bool& created) {
    created = false;
#ifdef _WIN32
    int fd = -1;
    (void)_wsopen_s(&fd, p.c_str(), _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY,
                   _SH_DENYRW, _S_IREAD | _S_IWRITE);
    if (fd < 0) return nullptr;
    created = true;
    auto* f = _fdopen(fd, "w+b");
    if (!f) _close(fd);
#else
    int flags = O_CREAT | O_EXCL | O_RDWR;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = open(p.c_str(), flags, 0600);
    if (fd < 0) return nullptr;
    created = true;
    auto* f = fdopen(fd, "w+b");
    if (!f) close(fd);
#endif
    return f;
}
struct FileCloser { void operator()(std::FILE* f) const { if (f) std::fclose(f); } };
using File = std::unique_ptr<std::FILE, FileCloser>;
std::vector<std::string> Components(std::string name, bool directory) {
    if (name.empty() || name[0] == '/' || name.find('\\') != std::string::npos ||
        name.find(':') != std::string::npos || name.find('\0') != std::string::npos)
        throw ZipError("Unsafe ZIP entry path");
    if (directory && name.back() == '/') name.pop_back();
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= name.size()) {
        const auto end = name.find('/', start);
        auto part = name.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || part == "." || part == "..") throw ZipError("Ambiguous or traversing ZIP path");
        for (unsigned char ch : part) if (ch < 32 || ch == 127) throw ZipError("Control character in ZIP path");
        parts.push_back(std::move(part));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return parts;
}
struct Node {
    std::string name;
    std::size_t parent{};
    bool directory{};
    bool hidden{};
    mz_uint zip_index{};
    std::uint64_t size{};
    mz_uint32 crc{};
    std::map<std::string, std::size_t, std::less<>> children;
};
using Recency = std::list<mz_uint>;
struct Cached { Path path; std::uint64_t size{}; Recency::iterator recency; };
class Directory;
class ZipFile;
VirtualDir MakeDirectory(const std::shared_ptr<Archive::State>& state, std::size_t id);
}

struct Archive::State {
    Options options;
    File input;
    mz_zip_archive zip{};
    bool initialized{};
    mutable std::mutex mutex;
    std::vector<Node> nodes;
    std::size_t exefs{}, romfs{};
    Stats stats;
    Path owned_cache;
    Path pending_path;
    bool pending_owned{};
    std::unordered_map<mz_uint, Cached> cache;
    // Oldest at the front. Cache hits splice an existing node without allocating;
    // eviction never scans all resident entries, even for many small files.
    Recency recency;
    std::uint64_t sequence{};

    ~State() {
        if (initialized) mz_zip_reader_end(&zip);
        // Delete only files created successfully by this object, never recurse.
        std::error_code ec;
        for (const auto& [id, entry] : cache) std::filesystem::remove(entry.path, ec);
        if (pending_owned) std::filesystem::remove(pending_path, ec);
        if (!owned_cache.empty()) std::filesystem::remove(owned_cache, ec); // only if empty
    }
    static size_t ReadArchive(void* opaque, mz_uint64 offset, void* out, size_t count) {
        auto* s = static_cast<State*>(opaque);
        if (Seek(s->input.get(), offset) != 0) return 0;
        const auto got = std::fread(out, 1, count, s->input.get());
        s->stats.archive_bytes_read += got;
        return got;
    }
    std::string MinizError() const { return mz_zip_get_error_string(zip.m_last_error); }
    void Init(const Path& path, Options opts) {
        options = std::move(opts);
        if (!options.cache_quota_bytes || !options.max_entry_bytes) throw ZipError("Zero cache or entry limit");
        input.reset(OpenRead(path));
        if (!input) throw ZipError("Cannot open ZIP read-only");
        std::error_code ec;
        const auto length = std::filesystem::file_size(path, ec);
        if (ec) throw ZipError("Cannot determine ZIP size");
        zip.m_pRead = ReadArchive;
        zip.m_pIO_opaque = this;
        if (!mz_zip_reader_init(&zip, length, MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY))
            throw ZipError("ZIP central directory: " + MinizError());
        initialized = true;
        nodes.push_back(Node{"", 0, true});
        std::unordered_set<std::string> explicit_names;
        const auto count = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat st{};
            if (!mz_zip_reader_file_stat(&zip, i, &st)) throw ZipError("Invalid ZIP central-directory entry");
            if (st.m_is_encrypted || !st.m_is_supported || (st.m_method != 0 && st.m_method != 8))
                throw ZipError("Encrypted or unsupported ZIP entry");
            // Empty input has CRC32 zero. This metadata mismatch is detectable
            // without treating a zero-length Read as an integrity check.
            if (!st.m_is_directory && st.m_uncomp_size == 0 && st.m_crc32 != MZ_CRC32_INIT)
                throw ZipError("Empty ZIP file has an invalid CRC32");
            const auto type = (st.m_external_attr >> 16) & 0170000;
            if (type && type != 0100000 && type != 0040000) throw ZipError("Unsupported ZIP entry type");
            const auto length_name = mz_zip_reader_get_filename(&zip, i, nullptr, 0);
            if (length_name < 2 || length_name > 4097) throw ZipError("Invalid or excessive ZIP filename length");
            std::vector<char> name(length_name);
            if (mz_zip_reader_get_filename(&zip, i, name.data(), length_name) != length_name)
                throw ZipError("ZIP filename read failed");
            std::string full(name.data(), length_name - 1);
            const auto parts = Components(full, st.m_is_directory != 0);
            std::string canonical;
            for (const auto& part : parts) { if (!canonical.empty()) canonical += '/'; canonical += part; }
            if (!explicit_names.insert(canonical).second) throw ZipError("Duplicate ZIP entry: " + canonical);
            if (st.m_local_header_ofs >= length || st.m_comp_size > length - st.m_local_header_ofs)
                throw ZipError("ZIP entry range exceeds archive");
            if (st.m_is_directory && st.m_uncomp_size != 0) throw ZipError("Nonempty ZIP directory entry");
            std::size_t parent = 0;
            for (std::size_t p = 0; p < parts.size(); ++p) {
                const bool is_dir = p + 1 < parts.size() || st.m_is_directory;
                auto it = nodes[parent].children.find(parts[p]);
                if (it == nodes[parent].children.end()) {
                    const auto id = nodes.size();
                    nodes[parent].children.emplace(parts[p], id);
                    nodes.push_back(Node{parts[p], parent, is_dir});
                    if (is_dir) ++stats.directories;
                    parent = id;
                } else {
                    parent = it->second;
                    if (!nodes[parent].directory || !is_dir) throw ZipError("ZIP file/directory path collision");
                }
            }
            if (!st.m_is_directory) {
                auto& node = nodes[parent];
                node.zip_index = i; node.size = st.m_uncomp_size; node.crc = st.m_crc32;
                if (node.size > std::numeric_limits<std::size_t>::max() ||
                    node.size > std::numeric_limits<std::uint64_t>::max() - stats.total_uncompressed_bytes)
                    throw ZipError("ZIP sizes overflow host limits");
                node.hidden = options.exclude_listing && parts.back() == ".listing";
                if (node.hidden) ++stats.excluded_listing_files;
                else { ++stats.files; stats.total_uncompressed_bytes += node.size; }
            }
        }
        auto child = [this](std::size_t parent, std::string_view name) {
            const auto it = nodes[parent].children.find(name);
            if (it == nodes[parent].children.end() || !nodes[it->second].directory)
                throw ZipError("ZIP must contain explicit base/exefs and base/romfs trees");
            return it->second;
        };
        const auto base = child(0, "base");
        exefs = child(base, "exefs"); romfs = child(base, "romfs");
    }
    void EnsureCache() {
        if (!owned_cache.empty()) return;
        std::error_code ec;
        if (options.cache_directory.empty() || !std::filesystem::is_directory(options.cache_directory, ec) || ec)
            throw ZipError("Cache parent must be an existing directory");
        static std::atomic<std::uint64_t> serial{};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            const auto candidate = options.cache_directory / ("eden-zip-owned-" +
                std::to_string(static_cast<std::uint64_t>(tick)) + "-" + std::to_string(++serial));
            ec.clear();
            if (std::filesystem::create_directory(candidate, ec)) { owned_cache = candidate; return; }
            if (ec && ec != std::errc::file_exists) throw ZipError("Cannot create owned cache directory");
        }
        throw ZipError("Cannot allocate unique cache directory");
    }
    void EvictFor(std::uint64_t size) {
        while (stats.cache_bytes > options.cache_quota_bytes - size) {
            if (recency.empty()) throw ZipError("Cache accounting inconsistency");
            const auto victim = cache.find(recency.front());
            if (victim == cache.end()) throw ZipError("Cache accounting inconsistency");
            std::error_code ec;
            if (!std::filesystem::remove(victim->second.path, ec) || ec)
                throw ZipError("Cannot remove owned cache spill");
            stats.cache_bytes -= victim->second.size;
            recency.pop_front(); cache.erase(victim); ++stats.evictions;
        }
    }
    struct Sink { std::FILE* file; std::uint64_t expected{}, written{}; mz_ulong crc{}; };
    static size_t WriteSpill(void* opaque, mz_uint64 offset, const void* bytes, size_t length) {
        auto& sink = *static_cast<Sink*>(opaque);
        if (offset != sink.written || length > sink.expected - sink.written) return 0;
        const auto put = std::fwrite(bytes, 1, length, sink.file);
        if (put != length) return 0;
        sink.crc = mz_crc32(sink.crc, static_cast<const unsigned char*>(bytes), length);
        sink.written += length; return length;
    }
    Path Materialize(const Node& node) {
        if (pending_owned) throw ZipError("Previous failed spill could not be removed; cache is blocked");
        if (const auto found = cache.find(node.zip_index); found != cache.end()) {
            recency.splice(recency.end(), recency, found->second.recency);
            return found->second.path;
        }
        if (node.size > options.max_entry_bytes || node.size > options.cache_quota_bytes)
            throw ZipError("Entry exceeds configured spill or cache quota");
        EnsureCache(); EvictFor(node.size);
        const Path path = owned_cache / ("entry-" + std::to_string(node.zip_index) + "-" +
                                        std::to_string(++sequence) + ".spill");
        pending_path = path;
        File spill(OpenExclusive(path, pending_owned));
        auto discard = [this] {
            std::error_code ec;
            if (pending_owned && std::filesystem::remove(pending_path, ec) && !ec) {
                pending_owned = false; pending_path.clear();
            }
        };
        if (!spill) { discard(); throw ZipError("Cannot create exclusive cache spill"); }
        Sink sink{spill.get(), node.size, 0, MZ_CRC32_INIT};
        const bool extracted = mz_zip_reader_extract_to_callback(&zip, node.zip_index, WriteSpill, &sink, 0) != 0;
        const bool complete = extracted && sink.written == node.size && sink.crc == node.crc;
        const bool flushed = std::fflush(spill.get()) == 0;
        const auto close_result = std::fclose(spill.release());
        if (!complete || !flushed || close_result != 0) {
            discard();
            throw ZipError("Entry spill/CRC verification failed: " + MinizError());
        }
        try { recency.push_back(node.zip_index); }
        catch (...) { discard(); throw; }
        try {
            const bool inserted = cache.emplace(
                node.zip_index, Cached{path, node.size, std::prev(recency.end())}).second;
            if (!inserted) throw ZipError("Duplicate cache publication");
        } catch (...) { recency.pop_back(); discard(); throw; }
        pending_owned = false; pending_path.clear();
        stats.cache_bytes += node.size; ++stats.extractions;
        return path;
    }
    std::size_t Read(std::size_t id, u8* output, std::size_t length, std::size_t offset) {
        std::lock_guard lock(mutex);
        const auto& node = nodes[id];
        if (!length || offset >= node.size) return 0;
        if (!output) throw ZipError("NULL read destination");
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(length, node.size - offset));
        const auto path = Materialize(node);
        File spill(OpenRead(path));
        if (!spill || Seek(spill.get(), offset) != 0) throw ZipError("Cannot open/seek verified spill");
        const auto got = std::fread(output, 1, count, spill.get());
        if (got != count) throw ZipError("Short read from verified cache spill");
        return got;
    }
};

namespace {
class ZipFile final : public VfsFile {
    std::shared_ptr<Archive::State> state; std::size_t id;
public:
    ZipFile(std::shared_ptr<Archive::State> s, std::size_t n) : state(std::move(s)), id(n) {}
    std::string GetName() const override { return state->nodes[id].name; }
    std::size_t GetSize() const override { return static_cast<std::size_t>(state->nodes[id].size); }
    VirtualDir GetContainingDirectory() const override { return MakeDirectory(state, state->nodes[id].parent); }
    bool IsReadable() const override { return true; }
    bool IsWritable() const override { return false; }
    bool Resize(std::size_t) override { return false; }
    bool Rename(std::string_view) override { return false; }
    std::size_t Write(const u8*, std::size_t, std::size_t) override { return 0; }
    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override {
        return state->Read(id, data, length, offset);
    }
};
class Directory final : public ReadOnlyVfsDirectory {
    std::shared_ptr<Archive::State> state; std::size_t id;
public:
    Directory(std::shared_ptr<Archive::State> s, std::size_t n) : state(std::move(s)), id(n) {}
    std::string GetName() const override { return state->nodes[id].name; }
    VirtualDir GetParentDirectory() const override {
        return id == 0 ? nullptr : MakeDirectory(state, state->nodes[id].parent);
    }
    VirtualFile GetFile(std::string_view name) const override {
        const auto it = state->nodes[id].children.find(name);
        return it != state->nodes[id].children.end() && !state->nodes[it->second].directory &&
            !state->nodes[it->second].hidden ?
            std::make_shared<ZipFile>(state, it->second) : nullptr;
    }
    VirtualDir GetSubdirectory(std::string_view name) const override {
        const auto it = state->nodes[id].children.find(name);
        return it != state->nodes[id].children.end() && state->nodes[it->second].directory ?
            MakeDirectory(state, it->second) : nullptr;
    }
    std::vector<VirtualFile> GetFiles() const override {
        std::vector<VirtualFile> out;
        for (const auto& [name, child] : state->nodes[id].children)
            if (!state->nodes[child].directory && !state->nodes[child].hidden)
                out.push_back(std::make_shared<ZipFile>(state, child));
        return out;
    }
    std::vector<VirtualDir> GetSubdirectories() const override {
        std::vector<VirtualDir> out;
        for (const auto& [name, child] : state->nodes[id].children)
            if (state->nodes[child].directory) out.push_back(MakeDirectory(state, child));
        return out;
    }
};
VirtualDir MakeDirectory(const std::shared_ptr<Archive::State>& state, std::size_t id) {
    return std::make_shared<Directory>(state, id);
}
}
Archive::Archive(std::shared_ptr<State> state) : state_(std::move(state)) {}
Archive::~Archive() = default;
std::shared_ptr<Archive> Archive::Open(const Path& path, Options options) {
    auto state = std::make_shared<State>(); state->Init(path, std::move(options));
    return std::shared_ptr<Archive>(new Archive(std::move(state)));
}
VirtualDir Archive::Root() const { return MakeDirectory(state_, 0); }
VirtualDir Archive::ExeFs() const { return MakeDirectory(state_, state_->exefs); }
VirtualDir Archive::RomFs() const { return MakeDirectory(state_, state_->romfs); }
Stats Archive::GetStats() const { std::lock_guard lock(state_->mutex); return state_->stats; }
} // namespace FileSys::ZipVfs
