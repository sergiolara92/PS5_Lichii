// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/host_memory.h"
#include "fastmem.h"
#include <optional>
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <unordered_map>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>
#include <x86intrin.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef PS5_NATIVE
extern "C" {
std::int64_t sceKernelGetDirectMemorySize();
std::int32_t sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t, std::size_t, int, std::int64_t*);
std::int32_t sceKernelReleaseDirectMemory(std::int64_t, std::size_t);
std::int32_t sceKernelMapDirectMemory(void**, std::size_t, int, int, std::int64_t, std::size_t);
std::int32_t sceKernelReserveVirtualRange(void**, std::size_t, int, std::size_t);
std::int32_t sceKernelEnableDmemAliasing();
int sceKernelGetPageTableStats(int*, int*, int*, int*);
}
#endif

// Self-test probe: a load that may fault, and the fallback a fault is sent to.
extern "C" {
std::uint64_t eden_fastmem_probe_load(const void* address, std::uint64_t* rsp);
void eden_fastmem_probe_load_fault();
void eden_fastmem_probe_load_resume();
void eden_fastmem_probe_fallback();
}
asm(R"(
    .text
    .p2align 4
    .globl eden_fastmem_probe_load, eden_fastmem_probe_load_fault, eden_fastmem_probe_load_resume
eden_fastmem_probe_load:
    movq %rsp, (%rsi)
eden_fastmem_probe_load_fault:
    movq (%rdi), %rax
eden_fastmem_probe_load_resume:
    ret
    .p2align 4
    .globl eden_fastmem_probe_fallback
eden_fastmem_probe_fallback:
    movabsq $0x5a17ed5a17ed5a17, %rax
    ret
)");

namespace Common {
void* AllocateMemoryPages(size_t size) noexcept;
void* AllocateGuestMemory(size_t size) noexcept;  // above the guest's address space (memory_pages.cpp)
// The same block backed where the guest's kernel uses it, and what goes with it (memory_pages.cpp).
void* AllocateGuestMemoryWhereUsed(size_t size) noexcept;
void ClearGuestMemory(std::uint8_t* at, std::uint64_t offset, std::size_t bytes, int fill) noexcept;
void CommitGuestMemory(std::uint64_t offset, size_t bytes) noexcept;
void FreeGuestMemory(void* base, size_t size) noexcept;
void FreeMemoryPages(void* base, size_t size) noexcept;
// The 64-bit window's aliases of the guest's Own slots (guest_memory_service.inc).
bool AliasGuestSlot(std::uintptr_t backing, std::uintptr_t window_at, bool writable) noexcept;
bool AliasGuestPage(std::uintptr_t backing, std::uintptr_t at, std::uintptr_t region_at, bool writable, bool write,
                    bool* promote, bool* slot_read_only) noexcept;
bool DropGuestAlias(std::uintptr_t backing, std::uintptr_t window_at) noexcept;
#ifdef PS5_NATIVE
std::int64_t DirectMemoryStart(const void* pointer) noexcept;
#endif
} // namespace Common

namespace {
constexpr std::size_t GuestPage = 0x1000;
constexpr std::size_t GuestPages = std::size_t{1} << 20; // the A32 address space
constexpr std::size_t Granule = 0x4000; // PS5 kernel page; host checks use the same groups
constexpr std::size_t PagesPerGranule = Granule / GuestPage;
constexpr std::size_t Chunks = GuestPages / PagesPerGranule;
constexpr std::size_t WindowBytes = GuestPages * GuestPage;
// Fully aliased 2 MiB blocks whose backing is contiguous and 2 MiB-aligned are mapped again
// as one 2 MiB mapping so the kernel can use a large page (fewer TLB misses).
constexpr std::size_t LargePage = 0x200000;
constexpr std::size_t ChunksPerLarge = LargePage / Granule;
constexpr std::size_t LargeBlocks = Chunks / ChunksPerLarge;
// One byte per guest page below the window: the JIT reads it at [r13 + page - MapBytes]
// (headless/checked-fastmem.cmake). The window starts on a 2 MiB boundary, so the
// reservation has slack for the alignment; a guard granule after the window stays reserved.
constexpr std::size_t MapBytes = GuestPages;
constexpr std::size_t ReserveBytes = 2 * LargePage + WindowBytes + Granule;
constexpr std::uint8_t Readable = 1, Writable = 2; // Eden's requested page access
constexpr std::uint8_t ReadBlocked = 1, WriteBlocked = 2, Blocked = 3; // map byte bits
constexpr std::uint64_t ProbeMarker = 0x5a17ed5a17ed5a17ull;

std::atomic<bool> window_requested{false};
std::atomic<bool> window64_requested{false};
std::atomic<std::uint64_t> window64_base{0};
struct Window64;
std::atomic<Window64*> window64_current{nullptr};
// Why window faults were not served, and what dropped aliases (EDEN_FASTMEM64_WHY).
struct Window64Counts {
    std::atomic<std::uint64_t> no_region, partial, inconsistent, blocked, blocked_write, unaligned, slot_refused, read_only;
    std::atomic<std::uint64_t> small, small_cap, small_regions, protect_pieces, promoted, unmapped_access;
    std::atomic<std::uint64_t> drop_map, drop_unmap, drop_protect;
    // TSC ticks inside the window's Map/Unmap/Protect (Eden's mapping and GPU cache marking, the
    // GPU thread's share) and inside Fault, and the ticks since the window was made.
    std::atomic<std::uint64_t> track_calls, track_tsc, protect_tsc, fault_tsc, start_tsc;
    // Why a piece was not shown: a page of it read-blocked, write-blocked for a write, its mapped
    // pages not in line in the backing.
    std::atomic<std::uint64_t> piece_read_blocked, piece_write_blocked, piece_scattered;
    // Pieces whose backing is not on a 16 KiB boundary (the console maps 16 KiB pages), by the 4 GiB
    // guest area (address >> 32), and pieces the guest memory service would not show.
    std::array<std::atomic<std::uint64_t>, 64> piece_misaligned;
    std::atomic<std::uint64_t> piece_alias_failed;
} window64_counts;
// The first scattered pieces refused: guest address and each page's backing page (from the
// guest memory's base, + 1; 0 = unmapped).
struct ScatterSample {
    std::atomic<std::uint64_t> guest{0};
    std::array<std::atomic<std::uint32_t>, 4> pages{};
};
std::array<ScatterSample, 8> scatter_samples{};
std::atomic<std::uint32_t> scatter_sampled{0};
struct Window64Timed {
    std::atomic<std::uint64_t>& total;
    const std::uint64_t start = __rdtsc();
    ~Window64Timed() { total.fetch_add(__rdtsc() - start, std::memory_order_relaxed); }
};
// The last reason each 2 MiB region of the window refused a fault (0: none yet), and for partly
// mapped regions how many pages were mapped: which guest areas keep sites off fastmem.
enum : std::uint8_t { RefusedNone, RefusedPartial, RefusedInconsistent, RefusedBlocked, RefusedBlockedWrite,
                      RefusedUnaligned, RefusedSlot, RefusedKinds };
constexpr std::size_t Window64Regions = std::size_t{1} << (38 - 21);
std::array<std::atomic<std::uint8_t>, Window64Regions> window64_refused{};
std::array<std::atomic<std::uint16_t>, Window64Regions> window64_partial_pages{};
// The first partly mapped pieces refused (EDEN_FASTMEM64_WHY): guest address of the access, and
// which of the region's pages are mapped around it (bit 0 = the piece's first page, 16 pages).
struct PartialSample {
    std::atomic<std::uint64_t> guest{0};
    std::atomic<std::uint32_t> around{0};
    std::atomic<std::uint32_t> mapped{0};
};
std::array<PartialSample, 16> partial_samples{};
std::atomic<std::uint32_t> partial_sampled{0};
std::atomic<bool> large_requested{true};
struct Counters {
    std::atomic<std::uint64_t> window, mapped_pages, aliased_chunks, large_blocks;
    std::atomic<std::uint64_t> map_calls, unmap_calls, protect_calls, kernel_calls, kernel_ns, failures;
} counters;
std::atomic<const std::uint8_t*> current_map{nullptr};

std::uint8_t Access(Common::MemoryPermission perms) {
    return (True(perms & Common::MemoryPermission::Read) ? Readable : 0) |
           (True(perms & Common::MemoryPermission::Write) ? Writable : 0);
}

[[noreturn]] void Fatal(const char* operation, std::size_t chunk, std::size_t count) {
    // A stale alias would expose freed memory to JIT code.
    std::printf("EDEN_FASTMEM_FATAL op=%s chunk=%zu count=%zu errno=%d\n", operation, chunk, count, errno);
    std::fflush(stdout);
    std::abort();
}

// Fault probe state; only the window self-test installs ProbeHandler.
struct Probe {
    std::uint64_t rsp;
    int signal;
    unsigned hits;
} probe;
void ProbeHandler(int signal, siginfo_t*, void* context) {
    auto& rip = Eden::Fastmem::ContextRip(context);
    if (rip != reinterpret_cast<std::uint64_t>(&eden_fastmem_probe_load_fault)) {
        std::signal(signal, SIG_DFL); // Not a probe: repeat the fault with the default action.
        return;
    }
    // The same fake call dynarmic's handler makes for a fastmem fault.
    auto& rsp = Eden::Fastmem::ContextRsp(context);
    probe.rsp = rsp;
    probe.signal = signal;
    ++probe.hits;
    rsp -= sizeof(std::uint64_t);
    *reinterpret_cast<std::uint64_t*>(rsp) = reinterpret_cast<std::uint64_t>(&eden_fastmem_probe_load_resume);
    rip = reinterpret_cast<std::uint64_t>(&eden_fastmem_probe_fallback);
}

// Guest pages [0, 4 GiB) aliased at 16 KiB granularity into one reserved window: a chunk
// is aliased (read-write) when its four guest pages map consecutive backing pages that
// start on a 16 KiB backing boundary; everything else stays reserved. Access control is
// the per-page map byte the JIT tests before each direct access, so protection changes
// (GPU tracking, guest permissions) are byte stores, never system calls or faults.
class Window {
public:
    std::uint8_t* base{}; // window start (fastmem arena); the map occupies the MiB below
#ifdef PS5_NATIVE
    using Backing = std::int64_t; // direct-memory start of the backing
#else
    using Backing = int; // shared file descriptor of the backing
#endif

    ~Window() {
        if (!reservation) return;
        current_map = nullptr;
        if (munmap(reservation, ReserveBytes) != 0) Fatal("release", 0, 0);
#ifdef PS5_NATIVE
        if (map_physical >= 0 && sceKernelReleaseDirectMemory(map_physical, MapBytes) != 0) Fatal("map-release", 0, 0);
#endif
        counters.window = 0;
        counters.mapped_pages = 0;
        counters.aliased_chunks = 0;
        counters.large_blocks = 0;
    }

    bool Create(std::uint8_t* backing_base, std::size_t backing_size, Backing backing_handle) {
        backing = backing_handle;
#ifdef PS5_NATIVE
        // RADV's PS5 winsys owns [0x2_0000_0000, 0x3_0000_0000) (its 32-bit window) and places
        // device memory with fixed mappings in [0x40_0000_0000, 0x80_0000_0000); a window there
        // lost its access map to the first GPU allocations. Stay between the two.
        void* address = reinterpret_cast<void*>(0x1000000000ull);
        const auto reserved = sceKernelReserveVirtualRange(&address, ReserveBytes, 0, LargePage);
        const auto placed = reinterpret_cast<std::uintptr_t>(address);
        if (reserved != 0 || placed < 0x300000000ull || placed + ReserveBytes > 0x4000000000ull) {
            std::printf("EDEN_FASTMEM_WINDOW reserve=%08x address=%p\n", unsigned(reserved), address);
            if (reserved == 0) munmap(address, ReserveBytes);
            return false;
        }
        reservation = static_cast<std::uint8_t*>(address);
        Place();
        (void)sceKernelEnableDmemAliasing(); // Reports zero regardless; the self-test checks.
        void* map_at = map;
        if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), MapBytes, Granule, 12, &map_physical) != 0 ||
            sceKernelMapDirectMemory(&map_at, MapBytes, PROT_READ | PROT_WRITE, MAP_FIXED, map_physical, Granule) != 0 ||
            map_at != map) {
            std::printf("EDEN_FASTMEM_WINDOW map=failed\n");
            return Abandon();
        }
#else
        void* address = mmap(nullptr, ReserveBytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (address == MAP_FAILED) return false;
        reservation = static_cast<std::uint8_t*>(address);
        Place();
        if (mmap(map, MapBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != map)
            return Abandon();
#endif
        std::memset(map, Blocked, MapBytes);
        try {
            page_backing.assign(GuestPages, 0);
            page_access.assign(GuestPages, 0);
            chunk_aliased.assign(Chunks, 0);
            chunk_granule.assign(Chunks, 0);
            block_large.assign(LargeBlocks, 0);
        } catch (const std::bad_alloc&) {
            std::printf("EDEN_FASTMEM_WINDOW state=allocation\n");
            return Abandon();
        }
        if (const char* failure = SelfTest(backing_base)) {
            std::printf("EDEN_FASTMEM_CHECK ok=0 step=%s errno=%d\n", failure, errno);
            return Abandon();
        }
        large = large_requested && LargeSelfTest(backing_base, backing_size);
        counters.window = reinterpret_cast<std::uintptr_t>(base);
        current_map = map;
        return true;
    }

    void Map(std::size_t offset, std::size_t host_offset, std::size_t length, std::uint8_t access) {
        std::size_t first, last;
        if (!Pages(offset, length, first, last)) return;
        std::lock_guard lock{mutex};
        ++counters.map_calls;
        const std::size_t host = host_offset / GuestPage;
        for (std::size_t p = first; p <= last; ++p) {
            if (!page_backing[p]) ++counters.mapped_pages;
            page_backing[p] = static_cast<std::uint32_t>(host + (p - first) + 1);
            page_access[p] = access;
        }
        Update(first / PagesPerGranule, last / PagesPerGranule);
    }

    void Unmap(std::size_t offset, std::size_t length) {
        std::size_t first, last;
        if (!Pages(offset, length, first, last)) return;
        std::lock_guard lock{mutex};
        ++counters.unmap_calls;
        for (std::size_t p = first; p <= last; ++p) {
            if (page_backing[p]) --counters.mapped_pages;
            page_backing[p] = 0;
            page_access[p] = 0;
        }
        Update(first / PagesPerGranule, last / PagesPerGranule);
    }

    void Protect(std::size_t offset, std::size_t length, std::uint8_t access) {
        std::size_t first, last;
        if (!Pages(offset, length, first, last)) return;
        std::lock_guard lock{mutex};
        ++counters.protect_calls;
        std::fill(page_access.begin() + first, page_access.begin() + last + 1, access);
        Refresh(first, last);
    }

private:
    std::mutex mutex;
    std::uint8_t* reservation{};
    std::uint8_t* map{};
#ifdef PS5_NATIVE
    std::int64_t map_physical = -1;
#endif
    Backing backing{};
    std::vector<std::uint32_t> page_backing; // backing page + 1; 0 = unmapped
    std::vector<std::uint8_t> page_access;   // Readable | Writable as Eden requested
    std::vector<std::uint8_t> chunk_aliased;
    std::vector<std::uint32_t> chunk_granule; // backing granule of an aliased chunk
    std::vector<std::uint8_t> block_large;    // 2 MiB block mapped as one large mapping
    bool large = false;                       // large mappings enabled and self-tested

    // The window on the first 2 MiB boundary with room for the map below it.
    void Place() {
        const auto start = reinterpret_cast<std::uintptr_t>(reservation) + MapBytes;
        base = reinterpret_cast<std::uint8_t*>((start + LargePage - 1) & ~std::uintptr_t(LargePage - 1));
        map = base - MapBytes;
    }

    bool Abandon() {
        munmap(reservation, ReserveBytes);
#ifdef PS5_NATIVE
        if (map_physical >= 0) sceKernelReleaseDirectMemory(map_physical, MapBytes);
        map_physical = -1;
#endif
        reservation = map = base = nullptr;
        return false;
    }

    // Inclusive guest page range of a request, clipped to the window.
    static bool Pages(std::size_t offset, std::size_t length, std::size_t& first, std::size_t& last) {
        if (offset >= WindowBytes || !length) return false;
        first = offset / GuestPage;
        last = (std::min(offset + length, WindowBytes) - 1) / GuestPage;
        return true;
    }

    bool Aliasable(std::size_t chunk, std::uint32_t& granule) const {
        const std::size_t page = chunk * PagesPerGranule;
        const std::uint32_t entry = page_backing[page];
        if (!entry || (entry - 1) % PagesPerGranule) return false;
        for (std::size_t i = 1; i < PagesPerGranule; ++i)
            if (page_backing[page + i] != entry + i) return false;
        granule = (entry - 1) / PagesPerGranule;
        return true;
    }

    // Blocked bits of a page on its own; pages past the window are blocked.
    std::uint8_t Own(std::size_t page) const {
        if (page >= GuestPages || !chunk_aliased[page / PagesPerGranule]) return Blocked;
        const auto access = page_access[page];
        return ((access & Readable) ? 0 : ReadBlocked) | ((access & Writable) ? 0 : WriteBlocked);
    }
    // A page's byte also carries its successor's blocks: direct accesses may cross into it.
    void Refresh(std::size_t first, std::size_t last) {
        for (std::size_t page = first ? first - 1 : 0; page <= last; ++page)
            map[page] = Own(page) | Own(page + 1);
    }
    void Block(std::size_t chunk, std::size_t count) {
        const std::size_t first = chunk * PagesPerGranule, last = (chunk + count) * PagesPerGranule - 1;
        for (std::size_t page = first ? first - 1 : 0; page <= last; ++page) map[page] = Blocked;
    }

    template<class Operation>
    bool Kernel(Operation&& operation) {
        const auto start = std::chrono::steady_clock::now();
        const bool ok = operation();
        counters.kernel_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        ++counters.kernel_calls;
        return ok;
    }
    bool MapChunks(std::size_t chunk, std::size_t count, std::uint32_t granule, std::size_t alignment = Granule) {
        auto* at = base + chunk * Granule;
        const std::size_t bytes = count * Granule;
        return Kernel([&] {
#ifdef PS5_NATIVE
            void* address = at;
            return sceKernelMapDirectMemory(&address, bytes, PROT_READ | PROT_WRITE, MAP_FIXED,
                                            backing + std::int64_t(granule) * std::int64_t(Granule),
                                            alignment) == 0 &&
                   address == at;
#else
            return mmap(at, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, backing,
                        off_t(granule) * off_t(Granule)) == at;
#endif
        });
    }
    bool UnmapChunks(std::size_t chunk, std::size_t count) {
        auto* at = base + chunk * Granule;
        const std::size_t bytes = count * Granule;
        return Kernel([&] {
#ifdef PS5_NATIVE
            if (munmap(at, bytes) != 0) return false;
            void* address = at;
            return sceKernelReserveVirtualRange(&address, bytes, MAP_FIXED, Granule) == 0 && address == at;
#else
            return mmap(at, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE,
                        -1, 0) == at;
#endif
        });
    }

    // A fully aliased block with contiguous, 2 MiB-aligned backing becomes one 2 MiB mapping
    // over its 16 KiB ones. Its bytes are blocked meanwhile; the backing is the same, so a
    // direct access racing the remap reads the same memory.
    void Promote(std::size_t block) {
        if (!large || block_large[block]) return;
        const std::size_t chunk = block * ChunksPerLarge;
        const std::uint32_t granule = chunk_granule[chunk];
        if (granule % ChunksPerLarge) return;
        for (std::size_t i = 0; i < ChunksPerLarge; ++i)
            if (!chunk_aliased[chunk + i] || chunk_granule[chunk + i] != granule + i) return;
        Block(chunk, ChunksPerLarge);
        if (MapChunks(chunk, ChunksPerLarge, granule, LargePage)) {
            block_large[block] = 1;
            ++counters.large_blocks;
        } else {
            // Refused after the self-test passed: stay with 16 KiB mappings from now on.
            std::printf("EDEN_FASTMEM_LARGE refused chunk=%zu errno=%d\n", chunk, errno);
            ++counters.failures;
            large = false;
            if (!MapChunks(chunk, ChunksPerLarge, granule)) Fatal("restore", chunk, ChunksPerLarge);
        }
        Refresh(chunk * PagesPerGranule, (chunk + ChunksPerLarge) * PagesPerGranule - 1);
    }
    // Before a chunk of a large block is withdrawn, the block goes back to 16 KiB mappings,
    // so the kernel never has to split a large mapping.
    void Demote(std::size_t block) {
        if (!block_large[block]) return;
        const std::size_t chunk = block * ChunksPerLarge;
        Block(chunk, ChunksPerLarge);
        if (!MapChunks(chunk, ChunksPerLarge, chunk_granule[chunk])) Fatal("demote", chunk, ChunksPerLarge);
        block_large[block] = 0;
        --counters.large_blocks;
        Refresh(chunk * PagesPerGranule, (chunk + ChunksPerLarge) * PagesPerGranule - 1);
    }

    void Update(std::size_t first, std::size_t last) {
        // 1. Withdraw aliases that no longer match their pages; block their bytes first.
        std::size_t run = 0, count = 0;
        const auto withdraw = [&] {
            if (!count) return;
            for (std::size_t b = run / ChunksPerLarge; b <= (run + count - 1) / ChunksPerLarge; ++b) Demote(b);
            Block(run, count);
            if (!UnmapChunks(run, count)) Fatal("unmap", run, count);
            for (std::size_t c = run; c < run + count; ++c) chunk_aliased[c] = 0;
            counters.aliased_chunks -= count;
            count = 0;
        };
        for (std::size_t c = first; c <= last; ++c) {
            std::uint32_t granule = 0;
            if (chunk_aliased[c] && (!Aliasable(c, granule) || granule != chunk_granule[c])) {
                if (count++ == 0) run = c;
            } else {
                withdraw();
            }
        }
        withdraw();

        // 2. Alias chunks that became aliasable, in runs of consecutive backing granules.
        std::uint32_t run_granule = 0;
        const auto alias = [&] {
            if (!count) return;
            if (MapChunks(run, count, run_granule)) {
                for (std::size_t i = 0; i < count; ++i) {
                    chunk_aliased[run + i] = 1;
                    chunk_granule[run + i] = run_granule + static_cast<std::uint32_t>(i);
                }
                counters.aliased_chunks += count;
            } else {
                ++counters.failures; // Left reserved: accesses keep the page-table path.
            }
            count = 0;
        };
        for (std::size_t c = first; c <= last; ++c) {
            std::uint32_t granule = 0;
            const bool candidate = !chunk_aliased[c] && Aliasable(c, granule);
            if (candidate && count && granule == run_granule + count) {
                ++count;
                continue;
            }
            alias();
            if (candidate) {
                run = c;
                run_granule = granule;
                count = 1;
            }
        }
        alias();

        // 3. Publish access for every page of the updated chunks (and the page before).
        Refresh(first * PagesPerGranule, (last + 1) * PagesPerGranule - 1);

        // 4. Large mappings for the blocks that are now whole.
        for (std::size_t b = first / ChunksPerLarge; b <= last / ChunksPerLarge; ++b) Promote(b);
    }

    // The large-mapping steps on the first backing block, after the window self-test: map 2 MiB
    // over 16 KiB mappings, go back to 16 KiB, withdraw one chunk. A failure only turns large
    // mappings off.
    bool LargeSelfTest(std::uint8_t* backing_base, std::size_t backing_size) {
        const char* failure = backing_size < LargePage ? "backing-size" : nullptr;
        constexpr std::uint64_t pattern = 0x0fedcba987654321ull;
        auto* word = reinterpret_cast<volatile std::uint64_t*>(backing_base + LargePage - 8);
        const auto at = [&] { return *reinterpret_cast<volatile std::uint64_t*>(base + LargePage - 8); };
#ifdef PS5_NATIVE
        if (!failure && backing % std::int64_t(LargePage)) failure = "backing-alignment";
#endif
        if (!failure && !MapChunks(0, ChunksPerLarge, 0)) failure = "alias";
        if (!failure) {
            *word = pattern;
            if (!MapChunks(0, ChunksPerLarge, 0, LargePage)) failure = "large";
            else if (at() != pattern) failure = "large-read";
            else if (!MapChunks(0, ChunksPerLarge, 0)) failure = "demote";
            else if (!UnmapChunks(0, 1)) failure = "partial-unmap";
            else if (at() != pattern) failure = "neighbour-read";
            *word = 0;
            if (!UnmapChunks(0, ChunksPerLarge)) Fatal("large-test", 0, ChunksPerLarge);
        }
        std::printf("EDEN_FASTMEM_LARGE ok=%d step=%s errno=%d\n", failure ? 0 : 1, failure ? failure : "none",
                    failure ? errno : 0);
        counters.kernel_calls = 0;
        counters.kernel_ns = 0;
        return !failure;
    }

    // Exercise the kernel operations the window relies on, with the fake call the JIT
    // fault handler makes, before any guest code runs. Returns the failing step.
    const char* SelfTest(std::uint8_t* backing_base) {
        struct sigaction action{}, previous_segv{}, previous_bus{};
        action.sa_sigaction = ProbeHandler;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGSEGV, &action, &previous_segv) != 0) return "install";
        if (sigaction(SIGBUS, &action, &previous_bus) != 0) {
            sigaction(SIGSEGV, &previous_segv, nullptr);
            return "install";
        }
        const char* failure = SelfTestSteps(backing_base);
        sigaction(SIGBUS, &previous_bus, nullptr);
        sigaction(SIGSEGV, &previous_segv, nullptr);
        return failure;
    }
    const char* SelfTestSteps(std::uint8_t* backing_base) {
        constexpr std::uint64_t pattern = 0x0123456789abcdefull;
        auto* word = reinterpret_cast<volatile std::uint64_t*>(backing_base + Granule + 8);
        std::uint64_t rsp = 0;
        if (map[MapBytes - 1] != Blocked) return "map";
        // Reserved memory faults; the fake call returns after the faulting load.
        probe = {};
        if (eden_fastmem_probe_load(base, &rsp) != ProbeMarker || probe.hits != 1) return "reserved-fault";
        if (probe.rsp != rsp) return "context-rsp";
        const int signal = probe.signal;
        // Two chunks alias the first two backing granules.
        if (!MapChunks(0, 2, 0)) return "alias";
        *word = pattern;
        if (eden_fastmem_probe_load(base + Granule + 8, &rsp) != pattern || probe.hits != 1) return "alias-read";
        // Unmapping one chunk re-reserves it and keeps its neighbour.
        if (!UnmapChunks(0, 1)) return "partial-unmap";
        if (eden_fastmem_probe_load(base + 8, &rsp) != ProbeMarker || probe.hits != 2) return "unmapped-fault";
        if (eden_fastmem_probe_load(base + Granule + 8, &rsp) != pattern || probe.hits != 2)
            return "neighbour-read";
        if (!UnmapChunks(1, 1)) return "unmap";
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 16; ++i) eden_fastmem_probe_load(base + Granule + 8, &rsp);
        const auto fault_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count() / 16;
        if (probe.hits != 18) return "final-fault";
        *word = 0;
        std::printf("EDEN_FASTMEM_CHECK ok=1 window=%p signal=%d fault_ns=%lld kernel_calls=%llu\n",
                    static_cast<void*>(base), signal, static_cast<long long>(fault_ns),
                    static_cast<unsigned long long>(counters.kernel_calls.load()));
        counters.kernel_calls = 0;
        counters.kernel_ns = 0;
        return nullptr;
    }
};
} // namespace

namespace Eden::Fastmem {
void Request(bool enabled) noexcept { window_requested = enabled; }
bool Requested() noexcept { return window_requested; }
void RequestA64(bool enabled) noexcept { window64_requested = enabled; }
bool RequestedA64() noexcept { return window64_requested; }
std::uint64_t Window64Base() noexcept { return window64_base; }
void Window64Why(char* out, std::size_t size) noexcept {
    const auto& c = window64_counts;
    std::snprintf(out, size, "no_region=%llu partial=%llu inconsistent=%llu blocked=%llu blocked_write=%llu unaligned=%llu "
                  "slot_refused=%llu read_only=%llu small=%llu small_cap=%llu small_regions=%llu protect_pieces=%llu promoted=%llu unmapped_access=%llu drop_map=%llu drop_unmap=%llu "
                  "drop_protect=%llu track_calls=%llu track_mtsc=%llu protect_mtsc=%llu fault_mtsc=%llu run_mtsc=%llu "
                  "piece_read_blocked=%llu piece_write_blocked=%llu piece_scattered=%llu",
                  (unsigned long long)c.no_region, (unsigned long long)c.partial, (unsigned long long)c.inconsistent,
                  (unsigned long long)c.blocked, (unsigned long long)c.blocked_write, (unsigned long long)c.unaligned,
                  (unsigned long long)c.slot_refused, (unsigned long long)c.read_only, (unsigned long long)c.small,
                  (unsigned long long)c.small_cap, (unsigned long long)c.small_regions,
                  (unsigned long long)c.protect_pieces, (unsigned long long)c.promoted,
                  (unsigned long long)c.unmapped_access, (unsigned long long)c.drop_map, (unsigned long long)c.drop_unmap, (unsigned long long)c.drop_protect,
                  (unsigned long long)c.track_calls, (unsigned long long)(c.track_tsc / 1000000),
                  (unsigned long long)(c.protect_tsc / 1000000), (unsigned long long)(c.fault_tsc / 1000000),
                  (unsigned long long)((__rdtsc() - c.start_tsc) / 1000000),
                  (unsigned long long)c.piece_read_blocked, (unsigned long long)c.piece_write_blocked,
                  (unsigned long long)c.piece_scattered);
    // Regions by last refusal, then per kind the 4 GiB guest areas (address >> 32) holding them.
    static constexpr const char* kinds[RefusedKinds] = {"", "partial", "inconsistent", "blocked", "blocked_write", "unaligned", "slot"};
    std::size_t used = std::strlen(out);
    const auto put = [&](const char* format, auto... values) {
        if (used < size) used += std::snprintf(out + used, size - used, format, values...);
    };
    std::size_t partial_most = 0;  // partly mapped regions with at least 7/8 of their pages mapped
    for (std::uint8_t kind = 1; kind < RefusedKinds; ++kind) {
        std::array<std::uint32_t, 64> areas{};
        std::size_t total = 0;
        for (std::size_t region = 0; region < Window64Regions; ++region)
            if (window64_refused[region].load(std::memory_order_relaxed) == kind) {
                ++areas[(region << 21) >> 32];
                ++total;
                if (kind == RefusedPartial && window64_partial_pages[region].load(std::memory_order_relaxed) >= 448) ++partial_most;
            }
        if (!total) continue;
        put(" | %s_regions=%zu", kinds[kind], total);
        for (std::size_t area = 0; area < areas.size(); ++area)
            if (areas[area]) put(" %zx:%u", area, areas[area]);
    }
    put(" | partial_7of8=%zu | piece_alias_failed=%llu piece_misaligned", partial_most,
        (unsigned long long)c.piece_alias_failed.load());
    for (std::size_t area = 0; area < c.piece_misaligned.size(); ++area)
        if (const auto n = c.piece_misaligned[area].load()) put(" %zx:%llu", area, (unsigned long long)n);
    const std::uint32_t samples = std::min<std::uint32_t>(partial_sampled.load(), partial_samples.size());
    for (std::uint32_t i = 0; i < samples; ++i)
        put(" | partial %llx mapped=%u around=%04x", (unsigned long long)partial_samples[i].guest.load(),
            partial_samples[i].mapped.load(), partial_samples[i].around.load());
    const std::uint32_t scattered = std::min<std::uint32_t>(scatter_sampled.load(), scatter_samples.size());
    for (std::uint32_t i = 0; i < scattered; ++i)
        put(" | scattered %llx pages=%x,%x,%x,%x", (unsigned long long)scatter_samples[i].guest.load(),
            scatter_samples[i].pages[0].load(), scatter_samples[i].pages[1].load(), scatter_samples[i].pages[2].load(),
            scatter_samples[i].pages[3].load());
}
void RequestLarge(bool enabled) noexcept { large_requested = enabled; }
Stats WindowStats() noexcept {
    std::uint64_t direct_reads = 0, direct_writes = 0;
    if (const auto* map = current_map.load()) {
        for (std::size_t page = 0; page < GuestPages; ++page) {
            direct_reads += !(map[page] & ReadBlocked);
            direct_writes += !(map[page] & WriteBlocked);
        }
    }
    return {counters.window, counters.mapped_pages, counters.aliased_chunks, direct_reads, direct_writes,
            counters.map_calls, counters.unmap_calls, counters.protect_calls,
            counters.kernel_calls, counters.kernel_ns, counters.failures, counters.large_blocks};
}
} // namespace Eden::Fastmem

#ifndef PS5_NATIVE
namespace {
// PC measurement only (EDEN_HOST_A64_FASTMEM): upstream's Linux fastmem for a 64-bit game, the
// shared backing mapped at each guest address of a 512 GiB arena, protection by mprotect. No
// checked bytes and no CRAM: it shows what direct A64 accesses are worth before a console design.
constexpr std::size_t ArenaBytes = std::size_t{1} << 39;
struct Arena {
    std::uint8_t* base{};
    int fd = -1;
    bool Create(int backing) {
        void* at = mmap(nullptr, ArenaBytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (at == MAP_FAILED) return false;
        base = static_cast<std::uint8_t*>(at);
        fd = backing;
        return true;
    }
    ~Arena() {
        if (base) munmap(base, ArenaBytes);
    }
    static int Prot(std::uint8_t access) {
        return ((access & Readable) ? PROT_READ : 0) | ((access & Writable) ? PROT_WRITE : 0);
    }
    void Map(std::size_t offset, std::size_t host_offset, std::size_t length, std::uint8_t access) {
        if (offset >= ArenaBytes || length > ArenaBytes - offset) return;
        if (mmap(base + offset, length, Prot(access), MAP_SHARED | MAP_FIXED, fd, off_t(host_offset)) != base + offset)
            Fatal("arena-map", offset, length);
    }
    void Unmap(std::size_t offset, std::size_t length) {
        if (offset >= ArenaBytes || length > ArenaBytes - offset) return;
        if (mmap(base + offset, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) != base + offset)
            Fatal("arena-unmap", offset, length);
    }
    void Protect(std::size_t offset, std::size_t length, std::uint8_t access) {
        if (offset >= ArenaBytes || length > ArenaBytes - offset) return;
        if (mprotect(base + offset, length, Prot(access)) != 0) Fatal("arena-protect", offset, length);
    }
};
} // namespace
#endif

namespace {
// The 64-bit window: the page table's fastmem arena for 64-bit games, aliased lazily. A JIT access
// faults on a region that shows nothing; if all 512 pages of that 2 MiB guest region map one
// 2 MiB-aligned stretch of the guest's memory, readable and writable, and that stretch is an Own
// slot (guest_memory_service.inc), the slot's block is shown there as one large page and the access
// repeats. Otherwise the JIT's fallback runs and the site moves onto the page-table path. 2^38 bytes cover every guest address a 64-bit title
// uses (code, stacks, alias and heap regions all sit below it); the JIT range-checks to that.
constexpr std::size_t Window64Bytes = std::size_t{1} << 38;
struct Window64 {
    static constexpr std::size_t Region = 0x200000, RegionPages = Region / GuestPage;
    struct State {
        std::uintptr_t start = 0;            // the guest-memory address the region's first page maps to
        bool consistent = true;              // every mapped page agrees with `start`
        // When not: each page's backing page (from backing_base) + 1, 0 = none.
        std::unique_ptr<std::uint32_t[]> backing_pages;
        std::vector<std::uintptr_t> piece_backings; // one backing address per 2 MiB that pieces show
        bool small = false;                  // shows 16 KiB pieces (AliasGuestPage)
        std::bitset<RegionPages / 4> pieces; // which pieces are shown
        std::bitset<RegionPages / 4> pieces_slot_ro; // shown read-only for the slot's sake (never raised)
        std::array<std::uint8_t, RegionPages / 4> piece_protection{}; // each shown piece's PROT_* bits
        std::bitset<RegionPages> mapped, read_blocked, write_blocked; // Eden's access, page by page
    };
    std::uint8_t* base{};
    std::uintptr_t backing_base{};
    std::mutex mutex;
    std::unordered_map<std::uint32_t, State> regions;

    // Whether one more region may be shown in 16 KiB pieces: each costs one of the title's 16,384 CPU
    // page tables, which the game, the GPU driver and the window share. On the console a new one is
    // refused once fewer than PageTablesKept are free (races leave ~1,300-2,700 free; stacks shown
    // since Eden 0060 need ~100 more regions than the old fixed cap of 768 allowed).
    static constexpr std::uint64_t SmallRegionCap = 768;  // without a page-table count (the PC)
    static constexpr int PageTablesKept = 1024;
    static bool PageTablesLeft(std::uint64_t small_regions) {
#ifdef PS5_NATIVE
        int cpu_tables = 0, cpu_free = 0, gpu_tables = 0, gpu_free = 0;
        if (sceKernelGetPageTableStats(&cpu_tables, &cpu_free, &gpu_tables, &gpu_free) == 0)
            return cpu_free > PageTablesKept;
#endif
        return small_regions < SmallRegionCap;
    }
    void Drop(std::uint32_t region, State& state, std::atomic<std::uint64_t>& why) {
        if (!state.start) return;
        const auto window_at = reinterpret_cast<std::uintptr_t>(base) + region * Region;
        bool dropped = Common::DropGuestAlias(state.start, window_at);
        if (state.small) {
            // Pieces may come from other slots: a region off a 2 MiB boundary spans two, and a
            // scattered one (stacks, TLS pages carved from the heap) any number.
            for (const std::uintptr_t backing : state.piece_backings)
                dropped |= Common::DropGuestAlias(backing, window_at);
            state.piece_backings.clear();
            state.small = false;
            state.pieces.reset();
            state.pieces_slot_ro.reset();
            window64_counts.small_regions.fetch_sub(1, std::memory_order_relaxed);
        }
        if (dropped) why.fetch_add(1, std::memory_order_relaxed);
    }
    // The 16 KiB holding `address` on its own, when its four guest pages map in line with the
    // region's start and are readable (and writable, for a write; else it is shown read-only).
    bool ShowPiece(std::uint32_t region, State& state, std::uintptr_t address, bool write) {
        constexpr std::size_t Piece = 0x4000, PiecePages = Piece / GuestPage;
        const auto region_at = reinterpret_cast<std::uintptr_t>(base) + region * Region;
        const std::size_t first = (address - region_at) / Piece * PiecePages;
        bool writable = true, any = false;
        // A piece may hold unmapped pages beside its mapped ones (a thread's lone 4 KiB TLS page, a
        // stack's top below its guard): they show the backing that lines up with the region, which
        // only an access the guest's own page table would refuse can reach (a stack overflow).
        // Guest pages are 4 KiB and window pieces 16 KiB; refusing them sent ~250k accesses a
        // race to the page-table path, each first through a fault.
        static const bool strict = std::getenv("EDEN_FASTMEM64_STRICT_PIECES") != nullptr;
        for (std::size_t i = first; i < first + PiecePages; ++i) {
            if (!state.mapped[i] && !strict) continue;
            any = true;
            if (!state.mapped[i]) {
                return false;
            }
            if (state.read_blocked[i]) return window64_counts.piece_read_blocked.fetch_add(1, std::memory_order_relaxed), false;
            writable &= !state.write_blocked[i];
        }
        if (!state.mapped[(address - region_at) / GuestPage]) {
            // One sample per region, the first 16 regions (`mutex` is held: no race between them).
            const std::uint64_t guest = address - reinterpret_cast<std::uintptr_t>(base);
            auto n = partial_sampled.load(std::memory_order_relaxed);
            bool seen = false;
            for (std::uint32_t k = 0; k < std::min<std::uint32_t>(n, partial_samples.size()); ++k)
                seen |= (partial_samples[k].guest.load(std::memory_order_relaxed) >> 21) == (guest >> 21);
            if (!seen && n < partial_samples.size() && partial_sampled.compare_exchange_strong(n, n + 1)) {
                std::uint32_t around = 0;
                const std::size_t from = first >= 4 ? first - 4 : 0;
                for (std::size_t j = from; j < from + 16 && j < RegionPages; ++j)
                    around |= std::uint32_t{state.mapped[j]} << (j - from);
                partial_samples[n].guest = guest;
                partial_samples[n].around = around;
                partial_samples[n].mapped = static_cast<std::uint32_t>(state.mapped.count());
            }
            window64_counts.unmapped_access.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (!any) return false;
        if (write && !writable) return window64_counts.piece_write_blocked.fetch_add(1, std::memory_order_relaxed), false;
        std::uintptr_t backing = state.start + first * GuestPage;
        if (!state.consistent) {
            // The piece's mapped pages must lie in line in the backing.
            backing = 0;
            const auto scattered = [&] {
                window64_counts.piece_scattered.fetch_add(1, std::memory_order_relaxed);
                auto n = scatter_sampled.load(std::memory_order_relaxed);
                if (n < scatter_samples.size() && scatter_sampled.compare_exchange_strong(n, n + 1)) {
                    scatter_samples[n].guest = address - reinterpret_cast<std::uintptr_t>(base);
                    for (std::size_t k = 0; k < PiecePages; ++k)
                        scatter_samples[n].pages[k] = state.mapped[first + k] ? state.backing_pages[first + k] : 0;
                }
                return false;
            };
            for (std::size_t i = first; i < first + PiecePages; ++i) {
                if (!state.mapped[i]) continue;
                if (!state.backing_pages[i]) return scattered();
                const std::uintptr_t at = backing_base + std::uintptr_t(state.backing_pages[i] - 1) * GuestPage -
                                          (i - first) * GuestPage;
                if (backing && at != backing) return scattered();
                backing = at;
            }
        }
        auto& counts = window64_counts;
        if ((backing - backing_base) % Piece) {
            counts.piece_misaligned[((address - reinterpret_cast<std::uintptr_t>(base)) >> 32) % 64].fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (!state.small && !PageTablesLeft(counts.small_regions.load(std::memory_order_relaxed)))
            return counts.small_cap.fetch_add(1, std::memory_order_relaxed), false;
        bool promote = false;
        bool slot_read_only = false;
        if (!Common::AliasGuestPage(backing, region_at + first * GuestPage, region_at, writable, write, &promote, &slot_read_only)) {
            // A write to a slot without a block of its own: take one as a write to the backing
            // would (guest_memory_service.inc), then show it.
            const auto take = Eden::Fastmem::guest_write_fault.load(std::memory_order_acquire);
            if (!promote || !take || !take(reinterpret_cast<void*>(backing)) ||
                !Common::AliasGuestPage(backing, region_at + first * GuestPage, region_at, writable, write, &promote,
                                        &slot_read_only))
                return counts.piece_alias_failed.fetch_add(1, std::memory_order_relaxed), false;
            counts.promoted.fetch_add(1, std::memory_order_relaxed);
            // The promotion voided the region's other pieces (DropSlotAliases).
            state.pieces.reset();
            state.pieces_slot_ro.reset();
        }
        if (!state.small) {
            state.small = true;
            counts.small_regions.fetch_add(1, std::memory_order_relaxed);
        }
        state.pieces.set(first / PiecePages);
        state.pieces_slot_ro.set(first / PiecePages, slot_read_only);
        state.piece_protection[first / PiecePages] = writable && !slot_read_only ? PROT_READ | PROT_WRITE : PROT_READ;
        if (std::find_if(state.piece_backings.begin(), state.piece_backings.end(), [backing](std::uintptr_t b) {
                return (b >> 21) == (backing >> 21);
            }) == state.piece_backings.end())
            state.piece_backings.push_back(backing);
        counts.small.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    template<class Each>
    void ForRegions(std::size_t offset, std::size_t length, Each&& each) {
        if (offset >= Window64Bytes || !length) return;
        const std::size_t end = std::min(offset + length, Window64Bytes);
        for (std::size_t region = offset / Region; region * Region < end; ++region) {
            const std::size_t first = std::max(offset, region * Region), last = std::min(end, (region + 1) * Region);
            each(static_cast<std::uint32_t>(region), (first - region * Region) / GuestPage, (last - first) / GuestPage, first);
        }
    }
    void Map(std::size_t offset, std::size_t host_offset, std::size_t length, std::uint8_t access) {
        window64_counts.track_calls.fetch_add(1, std::memory_order_relaxed);
        Window64Timed timed{window64_counts.track_tsc};
        std::lock_guard lock{mutex};
        ForRegions(offset, length, [&](std::uint32_t region, std::size_t page, std::size_t pages, std::size_t first) {
            auto& state = regions[region];
            Drop(region, state, window64_counts.drop_map);
            const std::uintptr_t start = backing_base + host_offset + (first - offset) - (first - region * Region);
            if (state.mapped.none()) {
                state.start = start;
                state.consistent = true;
                state.backing_pages.reset();
            } else if (state.start != start && state.consistent) {
                state.consistent = false;
                state.backing_pages = std::make_unique<std::uint32_t[]>(RegionPages);
                for (std::size_t i = 0; i < RegionPages; ++i)
                    if (state.mapped[i])
                        state.backing_pages[i] = std::uint32_t((state.start + i * GuestPage - backing_base) / GuestPage + 1);
            }
            for (std::size_t i = page; i < page + pages; ++i) {
                if (!state.consistent)
                    state.backing_pages[i] = std::uint32_t((start + i * GuestPage - backing_base) / GuestPage + 1);
                state.mapped.set(i);
                state.read_blocked.set(i, !(access & Readable));
                state.write_blocked.set(i, !(access & Writable));
            }
        });
    }
    void Unmap(std::size_t offset, std::size_t length) {
        window64_counts.track_calls.fetch_add(1, std::memory_order_relaxed);
        Window64Timed timed{window64_counts.track_tsc};
        std::lock_guard lock{mutex};
        ForRegions(offset, length, [&](std::uint32_t region, std::size_t page, std::size_t pages, std::size_t) {
            const auto it = regions.find(region);
            if (it == regions.end()) return;
            Drop(region, it->second, window64_counts.drop_unmap);
            for (std::size_t i = page; i < page + pages; ++i) {
                it->second.mapped.reset(i);
                if (it->second.backing_pages) it->second.backing_pages[i] = 0;
            }
            if (it->second.mapped.none()) regions.erase(it);
        });
    }
    void Protect(std::size_t offset, std::size_t length, std::uint8_t access) {
        window64_counts.track_calls.fetch_add(1, std::memory_order_relaxed);
        Window64Timed timed{window64_counts.track_tsc}, protect_timed{window64_counts.protect_tsc};
        std::lock_guard lock{mutex};
        ForRegions(offset, length, [&](std::uint32_t region, std::size_t page, std::size_t pages, std::size_t) {
            const auto it = regions.find(region);
            if (it == regions.end()) return;
            State& state = it->second;
            // A region shown in pieces changes only the pieces the pages are in: GPU write tracking
            // protects pages hundreds of times a second, and voiding the region each time had every
            // piece fault in again (160k drops a race).
            if (!state.small && access != (Readable | Writable)) Drop(region, state, window64_counts.drop_protect);
            for (std::size_t i = page; i < page + pages; ++i) {
                state.read_blocked.set(i, !(access & Readable));
                state.write_blocked.set(i, !(access & Writable));
            }
            if (!state.small) return;
            const auto region_at = reinterpret_cast<std::uintptr_t>(base) + region * Region;
            for (std::size_t piece = page / 4; piece * 4 < page + pages; ++piece) {
                if (!state.pieces[piece]) continue;
                bool readable = true, writable = true;
                for (std::size_t i = piece * 4; i < piece * 4 + 4; ++i) {
                    readable &= !state.read_blocked[i];
                    writable &= !state.write_blocked[i];
                }
                writable &= !state.pieces_slot_ro[piece];
                const int protection = !readable ? PROT_NONE : writable ? PROT_READ | PROT_WRITE : PROT_READ;
                // Cache marking repeats protections a piece already has: each mprotect is a system call.
                if (state.piece_protection[piece] == protection) continue;
                state.piece_protection[piece] = static_cast<std::uint8_t>(protection);
                if (mprotect(reinterpret_cast<void*>(region_at + piece * 0x4000), 0x4000, protection) != 0) {
                    Drop(region, state, window64_counts.drop_protect);
                    return;
                }
                window64_counts.protect_pieces.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    // From the fault handler (a JIT thread, which never holds `mutex` while it runs guest code).
    // A region whose only blocked pages are write-blocked (GPU write tracking) is shown read-only:
    // reads go direct and a write faults again, to the page-table path that tracks it.
    bool Fault(std::uintptr_t address, bool write) {
        const auto at = reinterpret_cast<std::uintptr_t>(base);
        if (address < at || address >= at + Window64Bytes) return false;
        const auto region = static_cast<std::uint32_t>((address - at) / Region);
        Window64Timed timed{window64_counts.fault_tsc};
        std::lock_guard lock{mutex};
        const auto it = regions.find(region);
        auto& counts = window64_counts;
        if (it == regions.end()) return counts.no_region.fetch_add(1, std::memory_order_relaxed), false;
        State& state = it->second;
        const auto refuse = [&](std::atomic<std::uint64_t>& count, std::uint8_t kind) {
            if (ShowPiece(region, state, address, write)) return true;
            count.fetch_add(1, std::memory_order_relaxed);
            window64_refused[region].store(kind, std::memory_order_relaxed);
            return false;
        };
        if (!state.mapped.all()) {
            window64_partial_pages[region].store(static_cast<std::uint16_t>(state.mapped.count()), std::memory_order_relaxed);
            return refuse(counts.partial, RefusedPartial);
        }
        if (!state.consistent) return refuse(counts.inconsistent, RefusedInconsistent);
        if (state.read_blocked.any()) return refuse(counts.blocked, RefusedBlocked);
        const bool writable = state.write_blocked.none();
        if (write && !writable) return refuse(counts.blocked_write, RefusedBlockedWrite);
        if (state.start % Region) return refuse(counts.unaligned, RefusedUnaligned);
        if (Common::AliasGuestSlot(state.start, at + region * Region, writable)) {
            if (state.small) {
                state.small = false;
                state.pieces.reset();
                state.pieces_slot_ro.reset();
                counts.small_regions.fetch_sub(1, std::memory_order_relaxed);
            }
            if (!writable) counts.read_only.fetch_add(1, std::memory_order_relaxed);
            window64_refused[region].store(RefusedNone, std::memory_order_relaxed);
            return true;
        }
        return refuse(counts.slot_refused, RefusedSlot);
    }
    bool Create() {
#ifdef PS5_NATIVE
        // Above the guest's own memory (0x80_0000_0000) and never in RADV's device-memory range
        // [0x40_0000_0000, 0x80_0000_0000), where it maps with fixed addresses.
        void* address = reinterpret_cast<void*>(0xa000000000ull);
        if (sceKernelReserveVirtualRange(&address, Window64Bytes, 0, LargePage) != 0) return false;
        const auto placed = reinterpret_cast<std::uintptr_t>(address);
        if (placed < 0x8000000000ull) {
            munmap(address, Window64Bytes);
            return false;
        }
#else
        void* address = mmap(nullptr, Window64Bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (address == MAP_FAILED) return false;
#endif
        base = static_cast<std::uint8_t*>(address);
        window64_base = reinterpret_cast<std::uintptr_t>(base);
        window64_counts.start_tsc = __rdtsc();
        return true;
    }
    ~Window64() {
        Eden::Fastmem::window64_fault.store(nullptr, std::memory_order_release);
        window64_current = nullptr;
        {
            std::lock_guard lock{mutex};
            for (auto& [region, state] : regions) Drop(region, state, window64_counts.drop_unmap);
            regions.clear();
        }
        if (base && munmap(base, Window64Bytes) != 0) std::abort();
        window64_base = 0;
    }
};
bool Window64Fault(void* address, bool write) noexcept {
    auto* window = window64_current.load(std::memory_order_acquire);
    return window && window->Fault(reinterpret_cast<std::uintptr_t>(address), write);
}
} // namespace

namespace Common {
// ponytail: backing plus, on request, the A32 fastmem window above. NCE is unavailable.
class HostMemory::Impl {
public:
    u8* base{};
    size_t size;
    std::unique_ptr<Window> window;
    std::unique_ptr<Window64> window64;
#ifndef PS5_NATIVE
    int fd = -1; // Host checks: a shared backing the window can alias.
    std::unique_ptr<Arena> arena; // EDEN_HOST_A64_FASTMEM (PC measurement)
#endif
    Impl(size_t requested, bool fastmem) : size(requested) {
#ifndef PS5_NATIVE
        if (fastmem) {
            fd = memfd_create("eden-backing", MFD_CLOEXEC);
            if (fd < 0 || ftruncate(fd, off_t(requested)) != 0)
                throw std::system_error(errno, std::generic_category(), "Backing file");
            void* mapped = mmap(nullptr, requested, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (mapped == MAP_FAILED) throw std::system_error(errno, std::generic_category(), "Backing map");
            base = static_cast<u8*>(mapped);
        }
#endif
        // The fastmem window aliases the block by its place in direct memory: it is then one block.
        if (!base)
            base = static_cast<u8*>(fastmem ? AllocateGuestMemory(requested) : AllocateGuestMemoryWhereUsed(requested));
        if (!base) throw std::system_error(errno, std::generic_category(), "Backing allocation");
        if (!fastmem) return;
#ifndef PS5_NATIVE
        if (std::getenv("EDEN_HOST_A64_FASTMEM")) {
            arena = std::make_unique<Arena>();
            if (!arena->Create(fd)) arena.reset();
            std::fprintf(stderr, "EDEN_A64_ARENA base=%p\n", arena ? static_cast<void*>(arena->base) : nullptr);
            return;
        }
#endif
        window = std::make_unique<Window>();
#ifdef PS5_NATIVE
        const Window::Backing handle = DirectMemoryStart(base);
#else
        const Window::Backing handle = fd;
#endif
        if (!window->Create(base, requested, handle)) window.reset();
    }
    ~Impl() {
        window.reset();
        window64.reset();
#ifndef PS5_NATIVE
        arena.reset();
#endif
#ifndef PS5_NATIVE
        if (fd >= 0) {
            if (munmap(base, size) != 0 || close(fd) != 0) std::abort();
            return;
        }
#endif
        FreeGuestMemory(base, size);
    }
};

HostMemory::HostMemory(size_t size, size_t virtual_size_)
    : backing_size(size),
      impl(std::make_unique<Impl>(size, virtual_size_ && Eden::Fastmem::Requested())),
      backing_base(impl->base) {
    if (impl->window) {
        virtual_base = impl->window->base;
        virtual_size = WindowBytes;
    }
#ifndef PS5_NATIVE
    if (impl->arena) {
        virtual_base = impl->arena->base;
        virtual_size = ArenaBytes;
    }
#endif
    if (virtual_size_ && !virtual_base && Eden::Fastmem::RequestedA64()) {
        impl->window64 = std::make_unique<Window64>();
        if (impl->window64->Create()) {
            virtual_base = impl->window64->base;
            virtual_size = Window64Bytes;
            impl->window64->backing_base = reinterpret_cast<std::uintptr_t>(backing_base);
            window64_current = impl->window64.get();
            Eden::Fastmem::window64_fault.store(&Window64Fault, std::memory_order_release);
        } else {
            impl->window64.reset();
        }
        std::printf("EDEN_FASTMEM64 window=%p\n", static_cast<void*>(virtual_base));
    }
}
HostMemory::~HostMemory() = default;
HostMemory::HostMemory(HostMemory&& other) noexcept { *this = std::move(other); }
HostMemory& HostMemory::operator=(HostMemory&& other) noexcept {
    if (this != &other) {
        impl = std::move(other.impl);
        backing_size = std::exchange(other.backing_size, 0);
        backing_base = std::exchange(other.backing_base, nullptr);
        virtual_base = std::exchange(other.virtual_base, nullptr);
        virtual_size = std::exchange(other.virtual_size, 0);
    }
    return *this;
}
// Lichii's HostMemory (2026-11) tracks used/dirty granules for its own zero-skip in ClearBackingRegion and can
// hint cold pages out. Here the guest memory service already skips slots that still show the zero block
// (ClearGuestMemory), so the bookkeeping has nothing to do and the hint is optional.
void HostMemory::HintCold(size_t, size_t, std::optional<size_t>) const {}
void HostMemory::MarkUsed(size_t, size_t) {}
void HostMemory::MarkDirty(size_t, size_t) {}
void HostMemory::ForgetContents() {}
void HostMemory::ClearBackingRegion(size_t offset, size_t length, u32 fill) {
    if (offset > backing_size || length > backing_size - offset)
        throw std::out_of_range("Backing clear range");
    // A fill with zeroes leaves a slot that shows the zero block alone (memory_pages.cpp).
    if (length) ClearGuestMemory(backing_base + offset, offset, length, static_cast<int>(fill));
}

#if EDEN_HOST_MEMORY_COMMIT_ON_USE
// Fritchii's commit-on-use (Windows, EDEN_NX2_SEC_RESERVE): the guest memory service commits on use
// here (src/guest_memory_service.inc), so commit_on_use stays false and these do nothing.
void HostMemory::Commit(size_t, size_t) {}
void HostMemory::CommitAllBut(std::span<const std::pair<size_t, size_t>>) {}
HostMemory::CommitStats HostMemory::GetCommitStats() {
    return {};
}
#endif
// Eden calls these only while a page table uses VirtualBasePointer() as its fastmem
// arena. Without a window a map or a protect is rejected rather than presented as a no-op; an
// unmap has nothing to undo.
void HostMemory::Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms, bool) {
    if (impl && impl->window64) return impl->window64->Map(virtual_offset, host_offset, length, Access(perms));
#ifndef PS5_NATIVE
    if (impl && impl->arena) return impl->arena->Map(virtual_offset, host_offset, length, Access(perms));
#endif
    if (!impl || !impl->window) throw std::logic_error("Native aliases unavailable without a fastmem window");
    if (host_offset > backing_size || length > backing_size - host_offset)
        throw std::out_of_range("Backing map range");
    impl->window->Map(virtual_offset, host_offset, length, Access(perms));
}
void HostMemory::Unmap(size_t virtual_offset, size_t length, bool) {
    // Without a window there are no aliases, so there is nothing to unmap: a process's page table
    // finalized at shutdown still asked (every console session ended "Native aliases unavailable").
    if (impl && impl->window64) return impl->window64->Unmap(virtual_offset, length);
#ifndef PS5_NATIVE
    if (impl && impl->arena) return impl->arena->Unmap(virtual_offset, length);
#endif
    if (!impl || !impl->window) return;
    impl->window->Unmap(virtual_offset, length);
}
void HostMemory::Protect(size_t virtual_offset, size_t length, MemoryPermission perms) {
    if (impl && impl->window64) return impl->window64->Protect(virtual_offset, length, Access(perms));
#ifndef PS5_NATIVE
    if (impl && impl->arena) return impl->arena->Protect(virtual_offset, length, Access(perms));
#endif
    if (!impl || !impl->window) throw std::logic_error("Native protection unavailable without a fastmem window");
    impl->window->Protect(virtual_offset, length, Access(perms));
}
void HostMemory::EnableDirectMappedAddress() {
    throw std::logic_error("NCE unavailable in x64 backing-only adapter");
}
} // namespace Common
