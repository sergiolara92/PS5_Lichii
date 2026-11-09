// SPDX-License-Identifier: GPL-3.0-or-later
// GPU command capture (coordination/notes/gpu-replay.md, gpu-replay-state.md). PC build only:
//   EDEN_HOST_GPU_CAPTURE=<file> EDEN_HOST_GPU_CAPTURE_AT=<submission> EDEN_HOST_GPU_CAPTURE_COUNT=<n>
// EDEN_HOST_GPU_CAPTURE_AFTER_S=<seconds> also waits that long. From the start it follows channels, GPU address spaces and device mappings (video_core/
// replay_hooks.h). On the GPU thread, before the first clean command list it executes from AT on (no macro collecting
// parameters, no inline upload, no pending acquire), with the GPU idle and everything the GPU wrote
// copied back to guest memory, it writes a snapshot: mappings, the physical pages the device maps,
// the engines' state and the shared native macro code; then COUNT submissions, each with the
// device-mapped pages the guest wrote since the previous one (soft-dirty bits), and the mapping
// changes and CPU-side cache notifications between them. Run with the split off (EDEN_NX2_SPLIT=0).

#include "gpu_replay.h"
#ifdef PS5_NATIVE
#include "performance.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <time.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <string>
#include <string_view>
#include <vector>
#include <fstream>
#include <fmt/format.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/cityhash.h"
#include "common/logging.h"
// Eglantine headers carry attributes this file's stricter warnings reject.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wignored-attributes"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/device_memory_manager.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/kepler_memory.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/host1x/host1x.h"
#include "video_core/nx2.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "video_core/replay_hooks.h"
#include "storage_paths.h"
#pragma clang diagnostic pop

namespace Common {
// src/memory_pages.cpp: guest memory backed where it is used takes writes only where committed.
void CommitGuestMemory(std::uint64_t offset, std::size_t bytes) noexcept;
#ifdef PS5_NATIVE
bool GuestMemoryReadsZero(const void* at) noexcept; // src/guest_memory_service.inc
#endif
} // namespace Common

namespace Eden {

namespace Hooks = VideoCommon::ReplayHooks;

// Record tags; each record is {u32 tag, u32 0, u64 bytes} then the payload.
enum class GpuReplayTag : u32 {
    Header = 1, Channel, AddressSpace, GmmuMap, GmmuUnmap, SmmuMap, SmmuUnmap, Pages, Engines,
    Shared, Push, CpuEvent, SnapshotEnd, End,
    Nx2Tables, ///< Sparse regions (which blocks were written) and semaphore regions
};

struct GpuReplayAccess {
    using Mm = Tegra::MemoryManager;
    using Smmu = Tegra::MaxwellDeviceMemoryManager;

    // Physical page (DeviceMemory backing offset >> 12) a device page resolves to, or ~0.
    static u64 PhysicalPage(const Smmu& smmu, u64 device_page) {
        const u32 compressed = smmu.tracked_entries[device_page].compressed_physical_ptr;
        return compressed != 0 ? u64{compressed - 1} : ~u64{0};
    }
    static u64 AddressBits(const Mm& mm) { return mm.address_space_bits; }
    static u64 BigPageBits(const Mm& mm) { return mm.big_page_bits; }
    static u64 PageBits(const Mm& mm) { return mm.page_bits; }
    static u64 SplitAddress(const Mm& mm) { return mm.split_address; }

    struct Blob {
        std::vector<u8> bytes;
        void Put(const void* data, size_t size) {
            const auto* p = static_cast<const u8*>(data);
            bytes.insert(bytes.end(), p, p + size);
        }
        template <typename T>
        void Value(const T& value) {
            static_assert(std::is_trivially_copyable_v<T>);
            Put(&value, sizeof(T));
        }
        void Words(const std::vector<u32>& words) {
            Value(static_cast<u64>(words.size()));
            Put(words.data(), words.size() * sizeof(u32));
        }
    };

    static bool Uploading(const Tegra::Engines::Upload::State& state) {
        return state.write_offset < state.copy_size; // a finished upload keeps its size
    }
    // Why this channel cannot be snapshotted at this boundary, or nullptr.
    static const char* NotClean(Tegra::Control::ChannelState& channel) {
        auto& p = *channel.payload;
        if (p.maxwell_3d.executing_macro != 0 || !p.maxwell_3d.macro_params.empty()) return "3D macro";
        if (Uploading(p.maxwell_3d.upload_state)) return "3D upload";
        if (!p.maxwell_3d.draw_manager.draw_state.inline_index_draw_indexes.empty()) return "inline index";
        if (p.kepler_compute.native_executing_macro || !p.kepler_compute.native_parameters.empty())
            return "compute macro";
        if (Uploading(p.kepler_compute.upload_state) || Uploading(p.kepler_memory.upload_state))
            return "compute upload";
        if (p.dma_pusher.puller.pending_acquire) return "pending acquire";
        if (p.dma_pusher.dma_state.method_count != 0) return "method data across lists";
        return nullptr;
    }

    static Blob Engines(Core::System& system, Tegra::Control::ChannelState& channel) {
        auto& p = *channel.payload;
        for (Tegra::Engines::EngineInterface* engine : std::array<Tegra::Engines::EngineInterface*, 5>{
                 &p.maxwell_3d, &p.fermi_2d, &p.kepler_compute, &p.maxwell_dma, &p.kepler_memory}) {
            engine->ConsumeSink(system);
        }
        Blob blob;
        auto& m = p.maxwell_3d;
        blob.Value(m.regs);
        blob.Value(m.shadow_state);
        blob.Value(m.macro_positions);
        blob.Value(m.native_data_ram);
        blob.Value(m.execute_on);
        blob.Value(m.draw_manager.draw_state.topology);
        blob.Value(m.draw_manager.draw_state.draw_mode);
        blob.Value(m.draw_manager.draw_state.draw_indexed);
        blob.Value(m.draw_manager.draw_state.base_index);
        blob.Value(m.draw_manager.draw_state.vertex_buffer);
        blob.Value(m.draw_manager.draw_state.index_buffer);
        blob.Value(m.draw_manager.draw_state.base_instance);
        blob.Value(m.draw_manager.draw_state.instance_count);
        blob.Value(m.draw_manager.instance_index);
        blob.Value(m.draw_manager.draw_texture_state);
        blob.Value(m.draw_manager.indirect_state);
        blob.Value(m.state);
        blob.Value(static_cast<u64>(m.macro_engine.uploaded_macro_code.size()));
        for (const auto& [method, code] : m.macro_engine.uploaded_macro_code) {
            blob.Value(method);
            blob.Words(code);
        }
        auto& k = p.kepler_compute;
        blob.Value(k.regs);
        blob.Value(k.native_registers);
        blob.Value(k.native_bind_pointer);
        blob.Value(k.native_compute_uploaded);
        blob.Value(k.upload_address);
        blob.Value(k.upload_dirty);
        blob.Value(p.kepler_memory.regs);
        blob.Value(p.fermi_2d.regs);
        blob.Value(p.maxwell_dma.regs);
        for (Tegra::Engines::EngineInterface* engine : std::array<Tegra::Engines::EngineInterface*, 5>{
                 &p.maxwell_3d, &p.fermi_2d, &p.kepler_compute, &p.maxwell_dma, &p.kepler_memory}) {
            blob.Value(engine->current_dma_segment);
            blob.Value(engine->current_dirty);
        }
        auto& d = p.dma_pusher;
        blob.Value(d.dma_state);
        blob.Value(static_cast<bool>(d.dma_increment_once));
        blob.Value(d.subchannel_type);
        blob.Value(d.split_method_end);
        blob.Value(d.puller.regs);
        blob.Value(d.puller.bound_engines);
        return blob;
    }

    static Blob Shared(Core::System& system, Tegra::Control::ChannelState& any_channel) {
        Blob blob;
        const auto& context = *any_channel.payload->maxwell_3d.native_macro_context;
        for (const Tegra::MacroEngine* engine : {&context.engine, &context.compute_engine}) {
            blob.Value(engine->native_code);
            blob.Value(engine->native_write_offset);
            blob.Value(engine->native_code_generation);
        }
        blob.Value(context.positions);
        blob.Value(context.compute_positions);
        auto& syncpoints = system.Host1x().GetSyncpointManager();
        for (const auto* values : {&syncpoints.syncpoints_guest, &syncpoints.syncpoints_host}) {
            for (const auto& value : *values) blob.Value(value.load());
        }
        return blob;
    }

    struct Reader {
        const u8* p;
        const u8* end;
        template <typename T>
        void Value(T& value) {
            static_assert(std::is_trivially_copyable_v<T>);
            std::memcpy(&value, p, sizeof(T));
            p += sizeof(T);
        }
        template <typename T>
        T Get() {
            T value;
            Value(value);
            return value;
        }
        std::vector<u32> Words() {
            const u64 n = Get<u64>();
            std::vector<u32> words(n);
            std::memcpy(words.data(), p, n * 4);
            p += n * 4;
            return words;
        }
    };

    static Tegra::Engines::EngineInterface* EngineOf(Tegra::Control::ChannelState::Payload& p,
                                                     Tegra::Engines::EngineTypes type) {
        using T = Tegra::Engines::EngineTypes;
        switch (type) {
        case T::Maxwell3D: return &p.maxwell_3d;
        case T::Fermi2D: return &p.fermi_2d;
        case T::KeplerCompute: return &p.kepler_compute;
        case T::MaxwellDMA: return &p.maxwell_dma;
        case T::KeplerMemory: return &p.kepler_memory;
        default: return nullptr;
        }
    }

    static void RestoreEngines(Core::System& system, Tegra::Control::ChannelState& channel, Reader in) {
        auto& p = *channel.payload;
        auto& m = p.maxwell_3d;
        in.Value(m.regs);
        in.Value(m.shadow_state);
        in.Value(m.macro_positions);
        in.Value(m.native_data_ram);
        in.Value(m.execute_on);
        in.Value(m.draw_manager.draw_state.topology);
        in.Value(m.draw_manager.draw_state.draw_mode);
        in.Value(m.draw_manager.draw_state.draw_indexed);
        in.Value(m.draw_manager.draw_state.base_index);
        in.Value(m.draw_manager.draw_state.vertex_buffer);
        in.Value(m.draw_manager.draw_state.index_buffer);
        in.Value(m.draw_manager.draw_state.base_instance);
        in.Value(m.draw_manager.draw_state.instance_count);
        in.Value(m.draw_manager.instance_index);
        in.Value(m.draw_manager.draw_texture_state);
        in.Value(m.draw_manager.indirect_state);
        in.Value(m.state);
        m.macro_engine.uploaded_macro_code.clear();
        for (u64 n = in.Get<u64>(); n > 0; --n) {
            const u32 method = in.Get<u32>();
            m.macro_engine.uploaded_macro_code[method] = in.Words();
        }
        auto& k = p.kepler_compute;
        in.Value(k.regs);
        in.Value(k.native_registers);
        in.Value(k.native_bind_pointer);
        in.Value(k.native_compute_uploaded);
        in.Value(k.upload_address);
        in.Value(k.upload_dirty);
        in.Value(p.kepler_memory.regs);
        in.Value(p.fermi_2d.regs);
        in.Value(p.maxwell_dma.regs);
        for (Tegra::Engines::EngineInterface* engine : std::array<Tegra::Engines::EngineInterface*, 5>{
                 &p.maxwell_3d, &p.fermi_2d, &p.kepler_compute, &p.maxwell_dma, &p.kepler_memory}) {
            in.Value(engine->current_dma_segment);
            in.Value(engine->current_dirty);
        }
        auto& d = p.dma_pusher;
        in.Value(d.dma_state);
        d.dma_increment_once = in.Get<bool>();
        in.Value(d.subchannel_type);
        in.Value(d.split_method_end);
        in.Value(d.puller.regs);
        in.Value(d.puller.bound_engines);
        // Engine pointers and the rasterizer's view follow the restored state.
        for (u32 i = 0; i < d.subchannel_type.size(); ++i) {
            if (auto* engine = EngineOf(p, d.subchannel_type[i])) d.BindSubchannel(engine, i, d.subchannel_type[i]);
        }
        auto* rasterizer = system.GPU().Renderer().ReadRasterizer();
        for (size_t stage = 0; stage < m.state.shader_stages.size(); ++stage) {
            for (u32 index = 0; index < m.state.shader_stages[stage].const_buffers.size(); ++index) {
                const auto& buffer = m.state.shader_stages[stage].const_buffers[index];
                if (buffer.enabled) rasterizer->BindGraphicsUniformBuffer(stage, index, buffer.address, buffer.size);
                else rasterizer->DisableGraphicsUniformBuffer(stage, index);
            }
        }
        m.dirty.flags.set();
    }

    static void RestoreShared(Core::System& system, Tegra::Control::ChannelState& any_channel, Reader in) {
        auto& context = *any_channel.payload->maxwell_3d.native_macro_context;
        for (Tegra::MacroEngine* engine : {&context.engine, &context.compute_engine}) {
            in.Value(engine->native_code);
            in.Value(engine->native_write_offset);
            in.Value(engine->native_code_generation);
        }
        in.Value(context.positions);
        in.Value(context.compute_positions);
        auto& syncpoints = system.Host1x().GetSyncpointManager();
        for (auto* values : {&syncpoints.syncpoints_guest, &syncpoints.syncpoints_host}) {
            for (auto& value : *values) value.store(in.Get<u32>());
        }
    }

    /// Whether the channel still has lists to parse or waits on a semaphore acquire.
    static bool Busy(Tegra::Control::ChannelState& channel) {
        auto& d = channel.payload->dma_pusher;
        return d.HasPendingAcquire() || !d.dma_pushbuffer.empty();
    }

    static void MapPhysical(Core::System& system, DAddr address, const u64* pages, size_t count) {
        system.Host1x().MemoryManager().MapPhysical(address, pages, count);
    }
};

namespace {

// CPU time of the calling thread: the GPU thread's own work, without its waits.
u64 ThreadCpuNs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<u64>(ts.tv_sec) * 1'000'000'000ULL + static_cast<u64>(ts.tv_nsec);
}

struct GmmuRange {
    u64 size;
    DAddr dev;
    u32 kind;
    bool big, sparse;
};

struct Recorder {
    Core::System* system{};
    std::FILE* out{};
    std::recursive_mutex mutex; ///< The GPU thread may map memory while it records
    u64 at{}, count{}, pushes{}, recorded{}, draws_at_snapshot{};
    std::chrono::steady_clock::time_point after; ///< EDEN_HOST_GPU_CAPTURE_AFTER_S from the start
    // EDEN_HOST_GPU_CAPTURE_TRIGGER=<file>: the snapshot waits until that file exists (the player
    // starts the capture where they are), looked for every half second.
    std::string trigger;
    std::chrono::steady_clock::time_point trigger_checked;
    bool triggered = true;
    u64 scan_ns{}; ///< Time spent finding changed pages (the console reads them all)
    enum class Phase { Follow, Window, Done } phase{Phase::Follow};
    std::vector<Tegra::Control::ChannelState*> channels;
    std::map<const Tegra::MemoryManager*, std::map<GPUVAddr, GmmuRange>> gmmu;
    std::map<DAddr, u64> smmu; ///< Device-mapped ranges
    int pagemap = -1;
    const u8* backing{};

    void Record(GpuReplayTag tag, const void* data, u64 size) {
        const u32 head[2]{static_cast<u32>(tag), 0};
        std::fwrite(head, sizeof(head), 1, out);
        std::fwrite(&size, sizeof(size), 1, out);
        if (size) std::fwrite(data, size, 1, out);
    }
    void Record(GpuReplayTag tag, const GpuReplayAccess::Blob& blob) {
        Record(tag, blob.bytes.data(), blob.bytes.size());
    }

    // Removes [start, start + size) from a range map (splitting ranges at its ends).
    template <typename Map, typename Split>
    static void Cut(Map& map, u64 start, u64 size, Split split) {
        const u64 end = start + size;
        auto it = map.lower_bound(start);
        if (it != map.begin() && std::prev(it)->first + split.Size(std::prev(it)->second) > start) --it;
        while (it != map.end() && it->first < end) {
            const u64 begin = it->first, finish = begin + split.Size(it->second);
            auto value = it->second;
            it = map.erase(it);
            if (begin < start) map.emplace(begin, split.Head(value, start - begin));
            if (finish > end) map.emplace(end, split.Tail(value, end - begin, finish - end));
        }
    }
    struct GmmuSplit {
        static u64 Size(const GmmuRange& r) { return r.size; }
        static GmmuRange Head(GmmuRange r, u64 size) { r.size = size; return r; }
        static GmmuRange Tail(GmmuRange r, u64 skip, u64 size) {
            if (!r.sparse) r.dev += skip;
            r.size = size;
            return r;
        }
    };
    struct SmmuSplit {
        static u64 Size(u64 size) { return size; }
        static u64 Head(u64, u64 size) { return size; }
        static u64 Tail(u64, u64, u64 size) { return size; }
    };

    // Device-mapped physical pages in [address, address + size), in device order.
    std::vector<u64> PhysicalPages(DAddr address, u64 size) {
        auto& smmu_manager = system->Host1x().MemoryManager();
        std::vector<u64> pages;
        for (u64 page = address >> 12; page < (address + size + 4095) >> 12; ++page)
            pages.push_back(GpuReplayAccess::PhysicalPage(smmu_manager, page));
        return pages;
    }

    // Content hash of each physical page last written to the capture (0: none yet): a page the
    // guest rewrote with the same bytes is not written again.
    std::vector<u64> hashes;

    // Pages record: {u64 page} then 4096 bytes, or {u64 page | 1 << 63} for a page of zeros.
    void WritePages(const std::vector<u64>& physical) {
        u64 last = 0;
        for (const u64 page : physical) if (page != ~u64{0}) last = std::max(last, page);
        if (last >= hashes.size()) hashes.resize(last + 1);
        // The pages of [begin, end) whose content changed, as a Pages payload. Chunks of a large
        // set run on several threads (the console reads every mapped page, a few GB).
        const auto scan = [this, &physical](size_t begin, size_t end) {
            static const u64 zero_hash = [] {
                static const std::array<char, 4096> zeros{};
                return Common::CityHash64(zeros.data(), zeros.size()) | 1;
            }();
            std::vector<u8> payload;
            for (size_t i = begin; i < end; ++i) {
                const u64 page = physical[i];
                if (page == ~u64{0}) continue;
                const u8* data = backing + page * 4096;
#ifdef PS5_NATIVE
                // Nothing of its own there: zeroes, and no fault to read them.
                const bool slot_zero = Common::GuestMemoryReadsZero(data);
#else
                const bool slot_zero = false;
#endif
                const u64 hash = slot_zero ? zero_hash : Common::CityHash64(reinterpret_cast<const char*>(data), 4096) | 1;
                if (hashes[page] == hash) continue;
                hashes[page] = hash;
                const bool zero = slot_zero || (data[0] == 0 && std::memcmp(data, data + 1, 4095) == 0);
                const u64 tagged = zero ? page | (u64{1} << 63) : page;
                payload.insert(payload.end(), reinterpret_cast<const u8*>(&tagged), reinterpret_cast<const u8*>(&tagged) + 8);
                if (!zero) payload.insert(payload.end(), data, data + 4096);
            }
            return payload;
        };
        // A Pages record per chunk of at most 16384 pages (64 MiB), written as it is made: the
        // console's snapshot reads every mapped page (GBs), and holding them all before writing ran
        // the app out of memory (the system ended it). Records of one submission may come in any
        // order: their pages are distinct.
        constexpr size_t chunk_pages = 16384, threads = 6;
        if (physical.size() <= chunk_pages) {
            const auto payload = scan(0, physical.size());
            if (!payload.empty()) Record(GpuReplayTag::Pages, payload.data(), payload.size());
            return;
        }
        const size_t chunks = (physical.size() + chunk_pages - 1) / chunk_pages;
        std::mutex record_mutex;
        std::vector<std::thread> workers;
        for (size_t t = 0; t < threads; ++t) {
            workers.emplace_back([&, t] {
                for (size_t chunk = t; chunk < chunks; chunk += threads) {
                    const auto payload = scan(chunk * chunk_pages, std::min(physical.size(), (chunk + 1) * chunk_pages));
                    if (payload.empty()) continue;
                    std::scoped_lock lock{record_mutex};
                    Record(GpuReplayTag::Pages, payload.data(), payload.size());
                }
            });
        }
        for (auto& worker : workers) worker.join();
    }

    // Device-mapped physical memory as runs of pages {first, count}, for the dirty scans.
    std::vector<std::pair<u64, u64>> mapped_runs;
    bool runs_stale = true;
    void RefreshRuns() {
        std::vector<u64> pages;
        for (const auto& [address, size] : smmu)
            for (const u64 page : PhysicalPages(address, size)) if (page != ~u64{0}) pages.push_back(page);
        std::sort(pages.begin(), pages.end());
        pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
        mapped_runs.clear();
        for (const u64 page : pages) {
            if (!mapped_runs.empty() && mapped_runs.back().first + mapped_runs.back().second == page) ++mapped_runs.back().second;
            else mapped_runs.emplace_back(page, 1);
        }
        runs_stale = false;
    }

    // Device-mapped physical pages the guest wrote since the last clear (pagemap bit 55), read a
    // run at a time. Without a pagemap (the console) every mapped page: WritePages keeps the changed.
    std::vector<u64> DirtyMappedPages() {
        if (runs_stale) RefreshRuns();
        std::vector<u64> dirty, entries;
        if (pagemap < 0) {
            for (const auto& [first, count] : mapped_runs)
                for (u64 i = 0; i < count; ++i) dirty.push_back(first + i);
            return dirty;
        }
        const u64 base = reinterpret_cast<std::uintptr_t>(backing) / 4096;
        for (const auto& [first, count] : mapped_runs) {
            entries.resize(count);
            const ssize_t got = pread(pagemap, entries.data(), count * 8, static_cast<off_t>((base + first) * 8));
            for (u64 i = 0; i < static_cast<u64>(got > 0 ? got : 0) / 8; ++i)
                if ((entries[i] >> 55) & 1) dirty.push_back(first + i);
        }
        return dirty;
    }
    static void ClearDirty() {
        if (std::FILE* f = std::fopen("/proc/self/clear_refs", "w")) {
            std::fputs("4", f);
            std::fclose(f);
        }
    }

    void Snapshot() {
        // On the GPU thread, between two command lists: what the GPU wrote goes back to guest
        // memory and every fence signals, so the snapshot holds all the state the next list sees.
        auto* rasterizer = system->GPU().Renderer().ReadRasterizer();
        for (const auto& [address, size] : smmu) rasterizer->FlushRegion(address, size, VideoCommon::CacheType::All);
        for (const auto& [mm, ranges] : gmmu) const_cast<Tegra::MemoryManager*>(mm)->FlushCaching();
        rasterizer->FlushCommands();
        rasterizer->WaitForIdle();
        rasterizer->ReleaseFences(true);
        // Before the pages are read: what the guest writes from now on is in the next delta.
        ClearDirty();
        const u64 header[2]{0x52504755474E4445ULL, 1}; // "EDENGUPR", version
        Record(GpuReplayTag::Header, header, sizeof(header));
        for (const auto& [mm, ranges] : gmmu) {
            const u64 as[5]{static_cast<u64>(mm->GetID()), GpuReplayAccess::AddressBits(*mm),
                            GpuReplayAccess::SplitAddress(*mm), GpuReplayAccess::BigPageBits(*mm),
                            GpuReplayAccess::PageBits(*mm)};
            Record(GpuReplayTag::AddressSpace, as, sizeof(as));
            for (const auto& [gpu_addr, r] : ranges) WriteGmmu(*mm, gpu_addr, r);
        }
        u64 total = 0;
        for (const auto& [address, size] : smmu) {
            WriteSmmu(address, size);
            total += size;
        }
        for (auto* channel : channels) {
            const s64 info[3]{channel->bind_id, static_cast<s64>(channel->program_id),
                              channel->memory_manager ? static_cast<s64>(channel->memory_manager->GetID()) : -1};
            Record(GpuReplayTag::Channel, info, sizeof(info));
            auto blob = GpuReplayAccess::Engines(*system, *channel);
            blob.bytes.insert(blob.bytes.begin(), reinterpret_cast<const u8*>(info), reinterpret_cast<const u8*>(info) + 8);
            Record(GpuReplayTag::Engines, blob);
        }
        if (!channels.empty()) Record(GpuReplayTag::Shared, GpuReplayAccess::Shared(*system, *channels.front()));
        {
            // Switch 2 tables the texture cache and puller read: without the sparse ones, sparse
            // images the game never wrote would be backed whole (GBs).
            std::vector<u64> t{Tegra::g_nx2_sparse_low.load(), Tegra::g_nx2_sparse_high.load()};
            for (const auto& region : Tegra::g_nx2_semaphore_regions) t.push_back(region.load());
            std::scoped_lock sparse_lock{Tegra::g_nx2_sparse_state_mutex};
            t.push_back(Tegra::g_nx2_sparse_state.size());
            for (const auto& [address, state] : Tegra::g_nx2_sparse_state) {
                t.insert(t.end(), {address, state.size, state.block_size, state.written.size()});
                u64 word = 0;
                for (size_t i = 0; i < state.written.size(); ++i) {
                    if (state.written[i]) word |= u64{1} << (i % 64);
                    if (i % 64 == 63 || i + 1 == state.written.size()) {
                        t.push_back(word);
                        word = 0;
                    }
                }
            }
            Record(GpuReplayTag::Nx2Tables, t.data(), t.size() * 8);
        }
        Record(GpuReplayTag::SnapshotEnd, nullptr, 0);
        draws_at_snapshot = Hooks::draws.load();
        std::fflush(out);
        LOG_INFO(Render, "GPU capture: snapshot at submission {}: {} channels, {} address spaces, {} MiB mapped",
                 pushes, channels.size(), gmmu.size(), total >> 20);
    }

    void WriteGmmu(const Tegra::MemoryManager& mm, GPUVAddr gpu_addr, const GmmuRange& r) {
        const u64 rec[6]{static_cast<u64>(mm.GetID()), gpu_addr, r.dev, r.size, r.kind,
                         u64{r.big} | (u64{r.sparse} << 1)};
        Record(GpuReplayTag::GmmuMap, rec, sizeof(rec));
    }
    void WriteSmmu(DAddr address, u64 size) {
        const auto physical = PhysicalPages(address, size);
        std::vector<u64> rec{address, size};
        rec.insert(rec.end(), physical.begin(), physical.end());
        Record(GpuReplayTag::SmmuMap, rec.data(), rec.size() * 8);
        WritePages(physical);
    }
};

Recorder* g_recorder{};

// Where the replay's pictures go: EDEN_GPU_REPLAY_DUMP_DIR, else the logs folder (on the PC the
// logs folder is the console's, so the PC runner passes its run folder).
std::string ReplayDumpPath(const std::string& name) {
    const char* dir = std::getenv("EDEN_GPU_REPLAY_DUMP_DIR");
    return dir != nullptr ? std::string{dir} + "/" + name : Eden::LogFile(name);
}

// Replay with the GPU thread split: RunInOrder work runs on the back (gpu_thread.cpp), so the
// front's CPU time is read where the front parses each list: at the window's first, half-way
// and last list (the last list's own time is left out). The other hooks do nothing.
std::atomic<u64> g_front_pushes{}, g_front_half_at{~u64{}};
std::array<std::atomic<u64>, 3> g_front_cpu{};
Hooks::Hooks g_replay_hooks{
    .channel = [](Tegra::Control::ChannelState&) {},
    .address_space = [](Tegra::MemoryManager&) {},
    .push = [](s32, const Tegra::CommandList&) {
        const u64 n = g_front_pushes.fetch_add(1, std::memory_order_relaxed);
        const u64 cpu = ThreadCpuNs();
        if (n == 0) g_front_cpu[0] = cpu;
        if (n == g_front_half_at.load(std::memory_order_relaxed)) g_front_cpu[1] = cpu;
        g_front_cpu[2] = cpu;
    },
    .gmmu_map = [](const Tegra::MemoryManager&, GPUVAddr, DAddr, u64, u32, bool, bool) {},
    .gmmu_unmap = [](const Tegra::MemoryManager&, GPUVAddr, u64) {},
    .smmu_map = [](DAddr, u64) {},
    .smmu_unmap = [](DAddr, u64) {},
    .cpu_event = [](Hooks::CpuEvent, DAddr, u64) {},
};

Hooks::Hooks g_hooks{
    .channel = [](Tegra::Control::ChannelState& channel) {
        std::scoped_lock lock{g_recorder->mutex};
        g_recorder->channels.push_back(&channel);
    },
    .address_space = [](Tegra::MemoryManager& mm) {
        std::scoped_lock lock{g_recorder->mutex};
        g_recorder->gmmu[&mm];
    },
    .push = [](s32 channel, const Tegra::CommandList& list) {
        auto& r = *g_recorder;
        std::scoped_lock lock{r.mutex};
        ++r.pushes;
        if (r.phase == Recorder::Phase::Follow && !r.triggered) {
            const auto now = std::chrono::steady_clock::now();
            if (now - r.trigger_checked >= std::chrono::milliseconds(500)) {
                r.trigger_checked = now;
                struct stat st{};
                if (stat(r.trigger.c_str(), &st) == 0) {
                    r.triggered = true;
                    r.at = r.pushes;
                    LOG_INFO(Render, "GPU capture: {} found at submission {}", r.trigger, r.pushes);
                }
            }
        }
        if (r.phase == Recorder::Phase::Follow && r.triggered && r.pushes >= r.at &&
            std::chrono::steady_clock::now() >= r.after) {
            for (auto* c : r.channels) {
                if (const char* why = GpuReplayAccess::NotClean(*c)) {
                    LOG_INFO(Render, "GPU capture: submission {} not clean ({}), trying the next", r.pushes, why);
                    return;
                }
            }
            r.Snapshot();
            r.phase = Recorder::Phase::Window;
        }
        if (r.phase != Recorder::Phase::Window) return;
        const u64 scan_start = ThreadCpuNs();
        r.WritePages(r.DirtyMappedPages());
        Recorder::ClearDirty();
        r.scan_ns += ThreadCpuNs() - scan_start;
        std::vector<u64> rec{static_cast<u64>(channel), list.command_lists.size(), list.prefetch_command_list.size(),
                             list.submitted_words.size(), list.submitted_offsets.size()};
        for (const auto& header : list.command_lists) rec.push_back(header.raw);
        std::vector<u8> bytes(reinterpret_cast<const u8*>(rec.data()), reinterpret_cast<const u8*>(rec.data() + rec.size()));
        const auto append = [&bytes](const void* p, size_t n) {
            bytes.insert(bytes.end(), static_cast<const u8*>(p), static_cast<const u8*>(p) + n);
        };
        append(list.prefetch_command_list.data(), list.prefetch_command_list.size() * 4);
        append(list.submitted_words.data(), list.submitted_words.size() * 4);
        append(list.submitted_offsets.data(), list.submitted_offsets.size() * 4);
        r.Record(GpuReplayTag::Push, bytes.data(), bytes.size());
        if (++r.recorded >= r.count) {
            // The draws of all lists but this last one, which runs after the hook.
            const u64 draws = Hooks::draws.load() - r.draws_at_snapshot;
            r.Record(GpuReplayTag::End, &draws, sizeof(draws));
            LOG_INFO(Render, "GPU capture: {} draws in the window", draws);
            std::fclose(r.out);
            r.out = nullptr;
            r.phase = Recorder::Phase::Done;
            LOG_INFO(Render, "GPU capture: {} submissions recorded, {:.1f} s finding changed pages",
                     r.recorded, static_cast<double>(r.scan_ns) / 1e9);
        }
    },
    .gmmu_map = [](const Tegra::MemoryManager& mm, GPUVAddr gpu_addr, DAddr dev, u64 size, u32 kind,
                   bool big, bool sparse) {
        auto& r = *g_recorder;
        std::scoped_lock lock{r.mutex};
        auto& ranges = r.gmmu[&mm];
        Recorder::Cut(ranges, gpu_addr, size, Recorder::GmmuSplit{});
        const GmmuRange range{size, dev, kind, big, sparse};
        ranges.emplace(gpu_addr, range);
        if (r.phase == Recorder::Phase::Window) r.WriteGmmu(mm, gpu_addr, range);
    },
    .gmmu_unmap = [](const Tegra::MemoryManager& mm, GPUVAddr gpu_addr, u64 size) {
        auto& r = *g_recorder;
        std::scoped_lock lock{r.mutex};
        Recorder::Cut(r.gmmu[&mm], gpu_addr, size, Recorder::GmmuSplit{});
        if (r.phase == Recorder::Phase::Window) {
            const u64 rec[3]{static_cast<u64>(mm.GetID()), gpu_addr, size};
            r.Record(GpuReplayTag::GmmuUnmap, rec, sizeof(rec));
        }
    },
    .smmu_map = [](DAddr address, u64 size) {
        auto& r = *g_recorder;
        std::scoped_lock lock{r.mutex};
        Recorder::Cut(r.smmu, address, size, Recorder::SmmuSplit{});
        r.smmu.emplace(address, size);
        r.runs_stale = true;
        if (r.phase == Recorder::Phase::Window) r.WriteSmmu(address, size);
    },
    .smmu_unmap = [](DAddr address, u64 size) {
        auto& r = *g_recorder;
        std::scoped_lock lock{r.mutex};
        Recorder::Cut(r.smmu, address, size, Recorder::SmmuSplit{});
        r.runs_stale = true;
        if (r.phase == Recorder::Phase::Window) {
            const u64 rec[2]{address, size};
            r.Record(GpuReplayTag::SmmuUnmap, rec, sizeof(rec));
        }
    },
    .cpu_event = [](Hooks::CpuEvent event, DAddr address, u64 size) {
        auto& r = *g_recorder;
        if (r.phase != Recorder::Phase::Window) return;
        std::scoped_lock lock{r.mutex};
        if (r.phase != Recorder::Phase::Window) return;
        const u64 rec[3]{static_cast<u64>(event), address, size};
        r.Record(GpuReplayTag::CpuEvent, rec, sizeof(rec));
    },
};

} // Anonymous namespace

int RunGpuReplay(Core::System& system, Core::Frontend::EmuWindow& window, const char* path) {
    // Read a record at a time (the console cannot map a file this large); a record's bytes stay
    // alive as long as the GPU thread work that uses them (shared_ptr).
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        std::fprintf(stderr, "GPU replay: cannot open %s\n", path);
        return 2;
    }
    std::fprintf(stderr, "GPU replay: %s opened\n", path);
    std::fflush(stderr);
    using Bytes = std::shared_ptr<std::vector<u8>>;
    const auto next_record = [file](u32& tag, Bytes& bytes) {
        u8 head[16];
        if (std::fread(head, 1, 16, file) != 16) return false;
        u64 n;
        std::memcpy(&tag, head, 4);
        std::memcpy(&n, head + 8, 8);
        bytes = std::make_shared<std::vector<u8>>(n);
        return n == 0 || std::fread(bytes->data(), 1, n, file) == n;
    };
    if (system.SetupForGpuReplay(window) != Core::SystemResultStatus::Success) {
        std::fprintf(stderr, "GPU replay: no GPU\n");
        return 2;
    }
    std::fprintf(stderr, "GPU replay: system set up\n");
    Tegra::g_nx2_ampere = true;
    auto& gpu = system.GPU();
    gpu.Start();
    std::fprintf(stderr, "GPU replay: GPU started\n");
    u8* const backing = system.DeviceMemory().buffer.BackingBasePointer();
    std::map<u64, std::shared_ptr<Tegra::MemoryManager>> spaces;
    std::map<s64, std::shared_ptr<Tegra::Control::ChannelState>> channels;
    // With the split, the barriers the player queues (RunInOrder) run on its back, past lists a
    // channel suspended on an acquire still holds on the front: before the half-way and final
    // marks, wait until every channel has parsed what it was sent (split off: they are idle).
    const auto wait_channels = [&gpu, &channels] {
        gpu.WaitForIdle();
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < limit) {
            bool busy = false;
            for (auto& [id, channel] : channels) busy = busy || GpuReplayAccess::Busy(*channel);
            if (!busy) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::fprintf(stderr, "GPU replay: a channel is still suspended after 30 s\n");
    };
    struct Pending {
        std::vector<std::pair<u64, const u8*>> pages; ///< physical page, data (nullptr: zeros)
        std::vector<std::array<u64, 3>> events;
        std::vector<Bytes> keep; ///< The records the pages point into
    };
    auto pending = std::make_shared<Pending>();
    const auto apply_pages = [backing](const u8* p, u64 n, std::vector<std::pair<u64, const u8*>>* defer) {
        for (const u8* end = p + n; p < end;) {
            u64 page;
            std::memcpy(&page, p, 8);
            p += 8;
            const bool zero = (page >> 63) != 0;
            page &= ~(u64{1} << 63);
            if (defer) defer->emplace_back(page, zero ? nullptr : p);
            else if (!zero) { // the snapshot's pages of zeros: fresh memory reads as zero already
                Common::CommitGuestMemory(page * 4096, 4096);
                std::memcpy(backing + page * 4096, p, 4096);
            }
            if (!zero) p += 4096;
        }
    };
    bool window_phase = false;
    std::chrono::steady_clock::duration player_wait{};
    u64 program_id = 0;
    // The window's second half on its own: the caches are warm by then, as in a race.
#ifdef PS5_NATIVE
    // The back's draw path by phase (performance.h, dev builds) and the split's census
    // (EDEN_NX2_STAGE3_CENSUS=1), for the window's second half: lever-stage3's measured split.
    using Perf = Eden::Performance::Totals;
    static constexpr std::array<std::pair<const char*, Perf*>, 15> draw_phases{{
        {"draw", &Eden::Performance::rasterizer_draw}, {"flush", &Eden::Performance::draw_flush},
        {"pipeline", &Eden::Performance::draw_pipeline}, {"lock", &Eden::Performance::draw_lock_wait},
        {"config", &Eden::Performance::draw_config}, {"state", &Eden::Performance::draw_state},
        {"record", &Eden::Performance::draw_record}, {"sync", &Eden::Performance::cfg_sync},
        {"read", &Eden::Performance::cfg_read}, {"handles", &Eden::Performance::cfg_handles},
        {"bind", &Eden::Performance::cfg_bind}, {"desc", &Eden::Performance::cfg_desc},
        {"stage_buffers", &Eden::Performance::cfg_stage_buffers}, {"rt", &Eden::Performance::cfg_rt},
        {"cfg_draw", &Eden::Performance::cfg_draw}}};
    static const auto census_now = [] {
        auto& c = VideoCommon::GpuSplit::census;
        return std::array<u64, 15>{c.engines.load(), c.sunk.load(), c.exec.load(), c.direct.load(),
                                   c.multi.load(), c.multi_words.load(), c.calls.load(), c.distinct.load(),
                                   c.direct_exec.load(), c.plain_distinct.load(),
                                   c.method_ns.load() / 1000, c.call_ns.load() / 1000,
                                   c.uniform_copies.load(), c.uniform_bytes.load(), c.uniform_ns.load() / 1000};
    };
#endif
    struct Half {
#ifdef PS5_NATIVE
        std::array<u64, draw_phases.size()> phase_ns{};
        std::array<u64, 15> census{};
#endif
        std::atomic<u64> cpu{}, draws{}, guest_cpu{}, guest_wall{};
        std::atomic<bool> on_back{}; ///< The split's back ran the work (gpu_split.h)
        std::array<u64, Hooks::wait_ns.size()> waits{}; ///< Hooks::wait_ns at the half
        u64 fence_waits{};
        std::chrono::steady_clock::time_point wall;
    };
    auto half = std::make_shared<Half>();
    // EDEN_GPU_REPLAY_HASH=1: after every 100th list, a checksum of what color target 0 shows,
    // to compare two runs of the capture (split on and off, before and after a change). Each one
    // waits for the GPU, so leave it off when timing.
    const bool hash_targets = std::getenv("EDEN_GPU_REPLAY_HASH") != nullptr;
    auto target_hashes = std::make_shared<std::vector<u64>>();
    // EDEN_GPU_REPLAY_DUMP=<n>: the picture at the n-th checksum too, as logs/replay-rt-<n>.ppm;
    // =all: every one, at a quarter of the size (the Pro's pictures differ by small noise from run
    // to run, so runs are compared with a tolerance, not by checksum).
    const char* const dump_env = std::getenv("EDEN_GPU_REPLAY_DUMP");
    const bool dump_all = dump_env != nullptr && std::string_view{dump_env} == "all";
    const u64 dump_at = dump_env != nullptr && !dump_all ? std::strtoull(dump_env, nullptr, 10) : 0;
    u64 draws_start = 0, draws_last = 0, lists_in_window = 0;
    // GPU-thread time spent applying the guest's writes: CPU-side work in the game, left out of
    // the GPU time this reports.
    auto guest_ns = std::make_shared<std::atomic<u64>>(0);
    auto guest_cpu_ns = std::make_shared<std::atomic<u64>>(0);
    auto gpu_cpu = std::make_shared<std::array<std::atomic<u64>, 2>>(); ///< GPU thread CPU at start, end
    // Lists read ahead of the GPU thread: their page data waits in memory, so only a few (the
    // console's heap cannot hold a whole window).
    auto in_flight = std::make_shared<std::atomic<u32>>(0);
    constexpr u32 MaxInFlight = 32;
    for (u8 head[16]; std::fread(head, 1, 16, file) == 16;) {
        u32 t;
        u64 len;
        std::memcpy(&t, head, 4);
        std::memcpy(&len, head + 8, 8);
        lists_in_window += t == static_cast<u32>(GpuReplayTag::Push);
        std::fseek(file, static_cast<long>(len), SEEK_CUR);
    }
    std::rewind(file);
    std::fprintf(stderr, "GPU replay: %llu command lists in the window; setting up the system\n",
                 static_cast<unsigned long long>(lists_in_window));
    std::fflush(stderr);
    g_front_half_at = lists_in_window / 2;
    Hooks::g_hooks = &g_replay_hooks;
    u64 pushes = 0;
    std::chrono::steady_clock::time_point start;
    u32 tag;
    for (Bytes record; next_record(tag, record);) {
        const u8* payload = record->data();
        const u64 n = record->size();
        const auto word = [payload](size_t i) {
            u64 v;
            std::memcpy(&v, payload + i * 8, 8);
            return v;
        };
        if (static u32 last = 0; tag != last && !window_phase) {
            std::fprintf(stderr, "GPU replay: record %u at %lld\n", tag, static_cast<long long>(std::ftell(file)));
            last = tag;
        }
        switch (static_cast<GpuReplayTag>(tag)) {
        case GpuReplayTag::AddressSpace: {
            auto mm = std::make_shared<Tegra::MemoryManager>(system, system.Host1x().MemoryManager(), word(1),
                                                             word(2), word(3), word(4));
            gpu.InitAddressSpace(*mm);
            spaces[word(0)] = mm;
            break;
        }
        // In the window, mapping changes go through the GPU thread's queue, in order: the caches
        // they touch belong to it (the emulator defers its unmaps the same way).
        case GpuReplayTag::GmmuMap: {
            auto* mm = spaces.at(word(0)).get();
            const bool big = word(5) & 1, sparse = (word(5) >> 1) & 1;
            const u64 gpu_addr = word(1), dev = word(2), size = word(3), kind = word(4);
            const auto map = [mm, big, sparse, gpu_addr, dev, size, kind] {
                if (sparse) mm->MapSparse(gpu_addr, size, big);
                else mm->Map(gpu_addr, dev, size, static_cast<Tegra::PTEKind>(kind), big);
            };
            if (window_phase) gpu.RunInOrder(map);
            else map();
            break;
        }
        case GpuReplayTag::GmmuUnmap: {
            auto* mm = spaces.at(word(0)).get();
            const u64 gpu_addr = word(1), size = word(2);
            gpu.RunInOrder([mm, gpu_addr, size] { mm->Unmap(gpu_addr, size); });
            break;
        }
        case GpuReplayTag::SmmuMap: {
            const u64 address = word(0), count = n / 8 - 2;
            const auto* pages = reinterpret_cast<const u64*>(payload + 16);
            if (window_phase) gpu.RunInOrder([&system, address, pages, count, record] { GpuReplayAccess::MapPhysical(system, address, pages, count); });
            else GpuReplayAccess::MapPhysical(system, address, pages, count);
            break;
        }
        case GpuReplayTag::SmmuUnmap: {
            const u64 address = word(0), size = word(1);
            gpu.RunInOrder([&system, address, size] { system.Host1x().MemoryManager().Unmap(address, size); });
            break;
        }
        case GpuReplayTag::Pages:
            apply_pages(payload, n, window_phase ? &pending->pages : nullptr);
            if (window_phase) pending->keep.push_back(record);
            break;
        case GpuReplayTag::Channel: {
            if (program_id == 0) program_id = word(1);
            auto channel = gpu.AllocateChannel();
            const s64 as = static_cast<s64>(word(2));
            if (as >= 0) channel->memory_manager = spaces.at(static_cast<u64>(as)).get();
            gpu.InitChannel(*channel, word(1));
            if (channel->bind_id != static_cast<s32>(word(0)))
                std::fprintf(stderr, "GPU replay: channel %lld is %d here\n", static_cast<long long>(word(0)), channel->bind_id);
            channels[static_cast<s64>(word(0))] = channel;
            break;
        }
        case GpuReplayTag::Engines: {
            // On the GPU thread with the channel bound: the rasterizer state it feeds is per channel.
            auto* channel = channels.at(static_cast<s64>(word(0))).get();
            const GpuReplayAccess::Reader in{payload + 8, payload + n};
            gpu.RunInOrder([&system, channel, in, record] {
                system.GPU().BindChannel(channel->bind_id);
                GpuReplayAccess::RestoreEngines(system, *channel, in);
            });
            break;
        }
        case GpuReplayTag::Shared: {
            auto* channel = channels.begin()->second.get();
            const GpuReplayAccess::Reader in{payload, payload + n};
            gpu.RunInOrder([&system, channel, in, record] { GpuReplayAccess::RestoreShared(system, *channel, in); });
            break;
        }
        case GpuReplayTag::Nx2Tables: {
            size_t i = 0;
            Tegra::g_nx2_sparse_low = word(i++);
            Tegra::g_nx2_sparse_high = word(i++);
            for (auto& region : Tegra::g_nx2_semaphore_regions) region = word(i++);
            std::scoped_lock sparse_lock{Tegra::g_nx2_sparse_state_mutex};
            Tegra::g_nx2_sparse_state.clear();
            for (u64 regions = word(i++); regions > 0; --regions) {
                const u64 address = word(i++), size = word(i++), block_size = word(i++), blocks = word(i++);
                Tegra::Nx2SparseRegionState state{size, block_size, std::vector<bool>(blocks)};
                for (u64 b = 0; b < blocks; b += 64) {
                    const u64 bits = word(i++);
                    for (u64 k = 0; k < 64 && b + k < blocks; ++k) state.written[b + k] = (bits >> k) & 1;
                }
                Tegra::g_nx2_sparse_state.emplace(address, std::move(state));
            }
            break;
        }
        case GpuReplayTag::SnapshotEnd:
            window_phase = true;
            // The pipelines the game loads from its shader cache before it plays (main.cpp): built
            // here as there, so the window's times leave out compiles a session does at load.
            if (const u64 program = program_id; program != 0) {
                std::fprintf(stderr, "GPU replay: loading the shader cache of %016llx\n",
                             static_cast<unsigned long long>(program));
                std::fflush(stderr);
                gpu.Renderer().ReadRasterizer()->LoadDiskResources(program, std::stop_token{},
                                                                  [](VideoCore::LoadCallbackStage, size_t, size_t) {});
            }
            gpu.WaitForIdle();
            {
                // With the split, the restores above ran on its back (RunInOrder): the front parses
                // the window's lists with the command parsers and engine bindings they restore.
                auto restored = std::make_shared<std::promise<void>>();
                auto finished = restored->get_future();
                gpu.RunInOrder([restored] { restored->set_value(); });
                finished.wait();
            }
            start = std::chrono::steady_clock::now();
            draws_start = Hooks::draws.load();
            gpu.RunInOrder([gpu_cpu] { (*gpu_cpu)[0] = ThreadCpuNs(); });
            std::fprintf(stderr, "GPU replay: snapshot restored (%zu channels, %zu address spaces)\n",
                         channels.size(), spaces.size());
            break;
        case GpuReplayTag::CpuEvent:
            pending->events.push_back({word(0), word(1), word(2)});
            break;
        case GpuReplayTag::Push: {
            const u64 channel = word(0), lists = word(1), prefetch = word(2), words = word(3), offsets = word(4);
            Tegra::CommandList list(lists);
            for (u64 i = 0; i < lists; ++i) list.command_lists[i].raw = word(5 + i);
            const u8* q = payload + (5 + lists) * 8;
            list.prefetch_command_list.resize(prefetch);
            std::memcpy(list.prefetch_command_list.data(), q, prefetch * 4);
            q += prefetch * 4;
            list.submitted_words.resize(words);
            std::memcpy(list.submitted_words.data(), q, words * 4);
            q += words * 4;
            list.submitted_offsets.resize(offsets);
            std::memcpy(list.submitted_offsets.data(), q, offsets * 4);
            // What the guest wrote before this list, then its cache notifications, on the GPU
            // thread just before the list (the order the capture saw them in).
            if (in_flight->load(std::memory_order_acquire) >= MaxInFlight) {
                // Time the player waits for the GPU thread: near zero means the replay is
                // bound by reading the capture, not by the GPU thread.
                const auto wait_begin = std::chrono::steady_clock::now();
                while (in_flight->load(std::memory_order_acquire) >= MaxInFlight) {
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
                player_wait += std::chrono::steady_clock::now() - wait_begin;
            }
            in_flight->fetch_add(1, std::memory_order_acq_rel);
            auto work_now = std::exchange(pending, std::make_shared<Pending>());
            // The guest's page writes land before the lists after them are parsed: on the split's
            // front (MapInOrder), which checks semaphore acquires against them; the back runs
            // ahead of a suspended front and an exact-value acquire missed the word it waited for.
            gpu.MapInOrder([backing, work = work_now] {
                for (const auto& [page, bytes] : work->pages) {
                    Common::CommitGuestMemory(page * 4096, 4096);
                    if (bytes) std::memcpy(backing + page * 4096, bytes, 4096);
                    else std::memset(backing + page * 4096, 0, 4096);
                }
            });
            gpu.RunInOrder([&system, guest_ns, guest_cpu_ns, in_flight, work = std::move(work_now)] {
                const auto begin = std::chrono::steady_clock::now();
                const u64 cpu_begin = ThreadCpuNs();
                auto* rasterizer = system.GPU().Renderer().ReadRasterizer();
                using E = Hooks::CpuEvent;
                for (const auto& [event, address, size] : work->events) {
                    switch (static_cast<E>(event)) {
                    case E::Write: rasterizer->OnCPUWrite(address, size); break;
                    case E::Invalidate: case E::InvalidatePreserve: rasterizer->InvalidateRegion(address, size); break;
                    case E::Flush: rasterizer->FlushRegion(address, size); break;
                    case E::Read: {
                        // As GPU::OnCPURead: only an area the caches did not already prepare is
                        // flushed (most reads find nothing to do).
                        const auto area = rasterizer->GetFlushArea(address, size);
                        if (!area.preemtive) rasterizer->FlushRegion(area.start_address, area.end_address - area.start_address);
                        break;
                    }
                    case E::FlushAndInvalidate: rasterizer->FlushAndInvalidateRegion(address, size); break;
                    }
                }
                guest_ns->fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - begin).count()));
                guest_cpu_ns->fetch_add(ThreadCpuNs() - cpu_begin);
                in_flight->fetch_sub(1, std::memory_order_acq_rel);
            });
            if (pushes == lists_in_window / 2) {
                wait_channels();
                gpu.RunInOrder([half, guest_cpu_ns, guest_ns] {
                    half->guest_wall = guest_ns->load();
                    half->cpu = ThreadCpuNs();
                    half->draws = Hooks::draws.load();
                    half->guest_cpu = guest_cpu_ns->load();
                    half->wall = std::chrono::steady_clock::now();
                    for (size_t i = 0; i < half->waits.size(); ++i) half->waits[i] = Hooks::wait_ns[i].load();
                    half->fence_waits = Hooks::fence_waits.load();
#ifdef PS5_NATIVE
                    for (size_t i = 0; i < draw_phases.size(); ++i) half->phase_ns[i] = draw_phases[i].second->nanoseconds.load();
                    half->census = census_now();
                    if (VideoCommon::GpuSplit::census_on) VideoCommon::GpuSplit::CensusReset();
#endif
                    // With the split this runs on the back (and must not send it work).
                    if (VideoCommon::GpuSplit::Get().Active()) half->on_back = true;
                });
            }
            if (pushes + 1 == lists_in_window) {
                // The capture's count stops before the last list: so does this one.
                gpu.RunInOrder([&draws_last] { draws_last = Hooks::draws.load(); });
            }
            gpu.PushGPUEntries(static_cast<s32>(channel), std::move(list));
            ++pushes;
            if (hash_targets && pushes % 100 == 0) {
                gpu.RunInOrder([&system, target_hashes, dump_at, dump_all] {
                    auto* const rasterizer = system.GPU().Renderer().ReadRasterizer();
                    if (dump_all) {
                        std::vector<u8> pixels;
                        u32 width{}, height{};
                        target_hashes->push_back(rasterizer->ReplayRenderTargetHash(&pixels, &width, &height));
                        if (pixels.size() < u64{width} * height * 4) return;
                        const u32 w = width / 4, h = height / 4;
                        std::ofstream out(ReplayDumpPath(fmt::format("replay-rt-{}.ppm", target_hashes->size())), std::ios::binary);
                        out << "P6\n" << w << ' ' << h << "\n255\n";
                        for (u32 y = 0; y < h; ++y) {
                            for (u32 x = 0; x < w; ++x) {
                                for (u32 c = 0; c < 3; ++c) {
                                    u32 sum = 0;
                                    for (u32 dy = 0; dy < 4; ++dy)
                                        for (u32 dx = 0; dx < 4; ++dx)
                                            sum += pixels[((u64{y} * 4 + dy) * width + x * 4 + dx) * 4 + c];
                                    out.put(static_cast<char>(sum / 16));
                                }
                            }
                        }
                        return;
                    }
                    if (target_hashes->size() + 1 != dump_at) {
                        target_hashes->push_back(rasterizer->ReplayRenderTargetHash());
                        return;
                    }
                    std::vector<u8> pixels;
                    u32 width{}, height{};
                    target_hashes->push_back(rasterizer->ReplayRenderTargetHash(&pixels, &width, &height));
                    // 4 bytes a pixel assumed (the race's targets are RGBA8); alpha dropped.
                    if (pixels.size() >= u64{width} * height * 4) {
                        std::ofstream out(ReplayDumpPath(fmt::format("replay-rt-{}.ppm", dump_at)), std::ios::binary);
                        out << "P6\n" << width << ' ' << height << "\n255\n";
                        for (u64 i = 0; i < u64{width} * height; ++i) out.write(reinterpret_cast<const char*>(&pixels[i * 4]), 3);
                    }
                });
            }
            break;
        }
        case GpuReplayTag::End: {
            // Done when the GPU thread has run every list (a barrier in its queue).
            wait_channels();
            auto done = std::make_shared<std::promise<void>>();
            auto finished = done->get_future();
            gpu.RunInOrder([done, gpu_cpu, half] {
                (*gpu_cpu)[1] = ThreadCpuNs();
                done->set_value();
            });
            finished.wait();
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            u64 captured_draws = 0;
            if (n >= 8) std::memcpy(&captured_draws, payload, 8);
            const double guest = static_cast<double>(guest_ns->load()) / 1e9;
            const double cpu = static_cast<double>((*gpu_cpu)[1] - (*gpu_cpu)[0] - guest_cpu_ns->load()) / 1e9;
            std::fprintf(stderr, "GPU replay: GPU thread CPU time %.2f s (%.2f ms per list)\n", cpu,
                         cpu * 1000.0 / static_cast<double>(pushes ? pushes : 1));
            const double half_cpu = static_cast<double>((*gpu_cpu)[1] - half->cpu - (guest_cpu_ns->load() - half->guest_cpu)) / 1e9;
            // Wall time of the GPU work: the guest's writes applied left out, as above.
            const double half_wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - half->wall).count() -
                                     static_cast<double>(guest_ns->load() - half->guest_wall) / 1e9;
            std::fprintf(stderr, "GPU replay: second half (%llu lists): GPU thread CPU %.3f s, wall %.3f s, %llu draws\n",
                         static_cast<unsigned long long>(pushes - lists_in_window / 2), half_cpu, half_wall,
                         static_cast<unsigned long long>(draws_last - half->draws));
            if (half->on_back) {
                // The numbers above are then the back's (the guest's writes are applied there too).
                std::fprintf(stderr, "GPU replay: split: the GPU thread CPU above is the back's; "
                             "the front's, second half %.3f s\n",
                             static_cast<double>(g_front_cpu[2] - g_front_cpu[1]) / 1e9);
            }
#ifdef PS5_NATIVE
            {
                // Per draw of the half, in us; "rest" is the GPU thread CPU above outside the draw path.
                const double half_draws = static_cast<double>(std::max<u64>(draws_last - half->draws, 1));
                std::string line;
                for (size_t i = 0; i < draw_phases.size(); ++i) {
                    line += fmt::format(" {}={:.3f}", draw_phases[i].first,
                                        static_cast<double>(draw_phases[i].second->nanoseconds.load() - half->phase_ns[i]) / 1e3 / half_draws);
                }
                const double draw_us = static_cast<double>(draw_phases[0].second->nanoseconds.load() - half->phase_ns[0]) / 1e3 / half_draws;
                std::fprintf(stderr, "GPU replay: second half, us per draw:%s rest=%.3f\n", line.c_str(),
                             half_cpu * 1e6 / half_draws - draw_us);
                if (VideoCommon::GpuSplit::census_on) {
                    static constexpr std::array<const char*, 15> names{"engines", "sunk", "exec", "direct", "multi",
                                                                       "multi_words", "calls", "distinct", "direct_exec",
                                                                       "plain_distinct", "method_us", "call_us",
                                                                       "uniform_copies", "uniform_bytes", "uniform_us"};
                    const auto now = census_now();
                    line.clear();
                    for (size_t i = 0; i < names.size(); ++i) {
                        line += fmt::format(" {}={:.2f}", names[i], static_cast<double>(now[i] - half->census[i]) / half_draws);
                    }
                    std::fprintf(stderr, "GPU replay: second half, back ring entries per draw:%s\n", line.c_str());
                    std::fprintf(stderr, "GPU replay: second half, back methods by time without a draw:%s\n",
                                 VideoCommon::GpuSplit::CensusTop(30).c_str());
                }
            }
#endif
            const auto waited = [&half](Hooks::Wait which) {
                const size_t i = static_cast<size_t>(which);
                return static_cast<double>(Hooks::wait_ns[i].load() - half->waits[i]) / 1e9;
            };
            std::fprintf(stderr, "GPU replay: second half, GPU thread blocked: suspended channels %.3f s, "
                         "host GPU %.3f s, Vulkan worker %.3f s, forced fence releases %.3f s (%llu)\n",
                         waited(Hooks::Wait::PendingSleep), waited(Hooks::Wait::HostGpu), waited(Hooks::Wait::Worker),
                         waited(Hooks::Wait::Fences),
                         static_cast<unsigned long long>(Hooks::fence_waits.load() - half->fence_waits));
            if (!target_hashes->empty()) {
                std::string line;
                for (const u64 hash : *target_hashes) line += fmt::format(" {:016x}", hash);
                std::fprintf(stderr, "GPU replay: color target checksums every 100 lists:%s\n", line.c_str());
            }
            std::fprintf(stderr, "GPU replay: the player waited %.2f s for the GPU thread (window)\n",
                         std::chrono::duration<double>(player_wait).count());
            std::fprintf(stderr, "GPU replay: %llu command lists, GPU work %.2f s (%.2f s with the guest's "
                         "writes applied), %llu draws (capture: %llu)\n",
                         static_cast<unsigned long long>(pushes), seconds - guest, seconds,
                         static_cast<unsigned long long>(draws_last - draws_start),
                         static_cast<unsigned long long>(captured_draws));
            std::fflush(stderr);
            std::fclose(file);
            return 0;
        }
        default:
            break;
        }
    }
    std::fclose(file);
    std::fprintf(stderr, "GPU replay: %s ended without an end record\n", path);
    return 1;
}

void StartGpuCapture(Core::System& system) {
    const char* path = std::getenv("EDEN_HOST_GPU_CAPTURE");
    if (path == nullptr || g_recorder != nullptr) return;
    auto* r = new Recorder;
    r->system = &system;
    r->out = std::fopen(path, "wb");
    r->at = std::strtoull(std::getenv("EDEN_HOST_GPU_CAPTURE_AT") ? std::getenv("EDEN_HOST_GPU_CAPTURE_AT") : "3000", nullptr, 10);
    r->count = std::strtoull(std::getenv("EDEN_HOST_GPU_CAPTURE_COUNT") ? std::getenv("EDEN_HOST_GPU_CAPTURE_COUNT") : "2100", nullptr, 10);
    const char* after = std::getenv("EDEN_HOST_GPU_CAPTURE_AFTER_S");
    r->after = std::chrono::steady_clock::now() + std::chrono::seconds(after ? std::strtoull(after, nullptr, 10) : 0);
    if (const char* trigger = std::getenv("EDEN_HOST_GPU_CAPTURE_TRIGGER")) {
        r->trigger = trigger;
        r->triggered = false;
    }
    r->backing = system.DeviceMemory().buffer.BackingBasePointer();
#ifdef PS5_NATIVE
    // No dirty-page bits on the console: every mapped page is read before each submission.
    const bool need_pagemap = false;
    if (r->out) std::setvbuf(r->out, nullptr, _IOFBF, 8 << 20);
#else
    r->pagemap = open("/proc/self/pagemap", O_RDONLY);
    const bool need_pagemap = true;
#endif
    if (r->out == nullptr || (need_pagemap && r->pagemap < 0)) {
        LOG_ERROR(Render, "GPU capture: cannot open {} or the pagemap", path);
        delete r;
        return;
    }
    g_recorder = r;
    Hooks::g_hooks = &g_hooks;
    LOG_INFO(Render, "GPU capture to {}: snapshot from submission {}, then {} submissions", path, r->at, r->count);
}

} // namespace Eden
