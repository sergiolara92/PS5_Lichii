// SPDX-License-Identifier: GPL-3.0-or-later
// ---- The guest's memory as a service: shared until written (included by memory_pages.cpp) ----
//
// After Gregory Price's "A Compressed RAM Service" (Linux Plumbers 2026, mm/cram.c): memory that
// is read where it lies and promoted when it is written. There the device compresses; here the
// emulator is the device, and the two forms a processor can read without a fault are a slot that
// shares the block of zeroes and a slot that shares a block with another slot of the same bytes.
//
//   Zero    nothing is mapped: the slot is only reserved
//   Shown   the slot shows the shared block of zeroes, read-only
//   Own     the slot has a block of its own, read-write
//   Shared  the slot shows a block it shares with other slots of the same bytes, read-only
//   Fine    the slot is 128 pages of 16 KiB, read-only: its pages of zeroes show the zero block, the
//           others a compact block that holds only them (a written slot with many pages of zeroes);
//           a write makes it whole again, and it waits longer each time before it may be Fine again
//   Cold    an Own slot nothing may touch until the next scan pass: its block stays, any access makes
//           it Own again (one fault); still Cold at the next pass, nothing touched it for one pass
//   Packed  a slot that was Cold for a whole pass: its block went back to the pool and its bytes are
//           kept LZ4-compressed; any access unpacks them into a new block (about 0.5 ms)
//
// Cold and Packed are the console's zram (dev-settings cram_pack=off turns them off): the pool the
// guest's memory shares with the graphics and the heap has no swap, and a game's memory holds many
// slots it touches rarely. A slot that does not compress to 3/4 stays Own (and waits 16 passes).
//
// Any access to a Zero slot faults, and HandleGuestWriteFault shows the zero block there; a write
// to a Shown or Shared slot faults, and it gives the slot a block of its own (zeroed, or a copy)
// and the write repeats. Every mapping of 2 MiB takes one of the title's CPU page tables, of which
// the console has 16,384 for everything a title maps (32 GiB, any memory type or protection):
// mapping all of a game's 20 GiB range left too few for its graphics, so a slot nothing has read
// keeps none. So memory is no longer taken when the guest's
// kernel hands pages out, only when they are written; a slot the guest's kernel clears or frees
// whole goes back to Zero; and a scanner gives back slots that hold only zeroes and
// lets slots of the same bytes share one block. A slot is locked (bit 7 of its state) while it
// changes; a writer that faults on a locked slot waits for it.
//
// 64-bit games only: a 32-bit game's fastmem window aliases the backing, which takes it whole.

namespace {
enum : std::uint8_t { SlotZero = 0, SlotOwn = 1, SlotShared = 2, SlotFine = 3, SlotShown = 4, SlotPacked = 5, SlotCold = 6,
                      SlotLock = 0x80, SlotMask = 0x7f };
// Reads as zeroes, with no memory of its own: Zero or Shown.
constexpr bool Empty(std::uint8_t state) noexcept { return state == SlotZero || state == SlotShown; }
// Readable where it lies with no fault: a block of its own, a shared one, or Fine's pages (`owned`).
constexpr bool Mapped(std::uint8_t state) noexcept { return state == SlotOwn || state == SlotShared || state == SlotFine; }
constexpr std::size_t FinePage = 16384, FinePages = SparseSlot / FinePage;
// A written slot becomes Fine with at least this many pages of zeroes (512 KiB given back).
constexpr std::size_t FineMinZeroPages = 32;
// A slot made whole again by a write waits 1, 2, 4 ... 1024 scan passes before it may be Fine again.
constexpr unsigned FineBackoffMax = 10;

// Duplicate sharing is off unless asked for (dev-settings cram_dedup=on): on the console it found a
// dozen 2 MiB pairs that were written apart within a minute, and with it on a large game held 20 FPS
// instead of 30 (the GPU worker waited for every fence, once a frame) where it held 30 without.
std::atomic<bool> service_requested{true}, scan_requested{true}, dedup_requested{false}, fine_requested{false};
std::atomic<bool> pack_requested{false}; // Cold and Packed (SetGuestMemoryPack)
std::atomic<bool> pack_works{true};      // the start-up check packed and unpacked a slot
// The pool's usable free memory (the console's; installed by the frontend): under 1 GiB, a pass every 2 s.
std::atomic<unsigned long long (*)()> pool_free{nullptr};
const char* pack_refused_step = "ok";
// Seconds a slot waits before it may be Cold again: touched while Cold (30), unpacked (60), did not
// compress (300). Counted in passes it made every hot slot fault each second under pressure: OOT on
// the Pro in Hyrule Field hitched 126-250 ms every few seconds with 5,327 slots made Cold in 13 passes.
constexpr std::uint32_t WarmWait = 30, UnpackWait = 60, RefusedWait = 300;
std::atomic<SparseRange*> service_range{nullptr}; // the guest range the service runs for
std::atomic<SparseRange*> probe_range{nullptr};   // the self-test's range
std::atomic<int> service_state{0};                // 0 not tested, 1 works here, 2 does not
const char* service_refused = "";

struct Counters {
    std::atomic<std::uint64_t> faults{0}, promotions{0}, copies{0}, waits{0};
    std::atomic<std::uint64_t> returns_clear{0}, returns_free{0}, returns_scan{0}, dedup{0};
    std::atomic<std::uint64_t> reserve_used{0}, failures{0}, passes{0}, scan_ns{0};
    std::atomic<std::uint64_t> fined{0}, unfined{0}, fine_failures{0}, shows{0};
    std::atomic<std::uint64_t> colds{0}, warms{0}, packs{0}, unpacks{0}, pack_refused{0}, retire_full{0}, sweeps{0};
    // Checks of packing: a window alias still recorded, bytes changed while compressing, bytes unpacked wrong.
    std::atomic<std::uint64_t> pack_aliased{0}, pack_changed{0}, unpack_wrong{0};
};
Counters counters;
void ResetCounters() noexcept {
    for (auto* counter : {&counters.faults, &counters.promotions, &counters.copies, &counters.waits,
                          &counters.returns_clear, &counters.returns_free, &counters.returns_scan, &counters.dedup,
                          &counters.reserve_used, &counters.failures, &counters.passes, &counters.scan_ns,
                          &counters.fined, &counters.unfined, &counters.fine_failures, &counters.shows,
                          &counters.colds, &counters.warms, &counters.packs, &counters.unpacks, &counters.pack_refused,
                          &counters.retire_full, &counters.sweeps, &counters.pack_aliased, &counters.pack_changed,
                          &counters.unpack_wrong})
        counter->store(0, std::memory_order_relaxed);
}

// ---- The 64-bit fastmem window's aliases of Own slots (src/host_memory.cpp, Window64) ----
// A window region (2 MiB) shows an Own slot's block, read-write, from a JIT fault until the slot
// changes. Every change of an Own slot (fenced for a scan, given back, made Fine or shared) first
// drops its aliases, under the slot's lock: a stale alias would reach memory the slot gave up.
constexpr unsigned AliasesPerSlot = 4;
std::atomic<std::uint64_t> aliases_made{0}, aliases_dropped{0}, aliases_refused{0};
// As VoidSlot: a reservation straight over the mapping (munmap first, then a fixed reservation,
// was refused with EINVAL on the console inside the window's reservation).
void VoidWindowRegion([[maybe_unused]] std::uintptr_t at) noexcept {
#ifdef PS5_NATIVE
    void* address = reinterpret_cast<void*>(at);
    if (sceKernelReserveVirtualRange(&address, SparseSlot, MAP_FIXED, SparseSlot) == 0 && address == reinterpret_cast<void*>(at))
        return;
    address = reinterpret_cast<void*>(at);
    if (munmap(address, SparseSlot) != 0 || sceKernelReserveVirtualRange(&address, SparseSlot, MAP_FIXED, SparseSlot) != 0 ||
        address != reinterpret_cast<void*>(at))
        std::abort(); // a window region left mapped would expose memory the slot gives up
#endif
}
// An entry is the window address, plus 1 when the region shows the block read-only, or 2 when it
// shows 16 KiB pieces of it (AliasGuestPage) rather than the whole block.
constexpr std::uintptr_t AliasReadOnly = 1, AliasSmall = 2, AliasFlags = AliasReadOnly | AliasSmall;
// Slot locked. The range's own table: it is sized with the range's other per-slot arrays (a table
// sized for the start-up probe's 4 GiB range was read past its end for the game's 20 GiB one).
void DropSlotAliases(SparseRange& range, std::size_t slot) noexcept {
    if (!range.aliases) return;
    for (unsigned i = 0; i < AliasesPerSlot; ++i)
        if (auto& alias = range.aliases[slot * AliasesPerSlot + i]) {
            VoidWindowRegion(alias & ~AliasFlags);
            alias = 0;
            aliases_dropped.fetch_add(1, std::memory_order_relaxed);
        }
}

// A Fine slot's 128 pages, bit set where the page shows the zero block (guest range only).
std::uint64_t* FineZero(SparseRange& range, std::size_t slot) noexcept {
    const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
    return reinterpret_cast<std::uint64_t*>(range.aliases + slots * AliasesPerSlot) + slot * 2;
}

bool LockSlot(SparseRange& range, std::size_t slot, bool count_wait = false) noexcept {
    auto& state = range.state[slot];
    bool waited = false;
    for (;;) {
        std::uint8_t value = state.load(std::memory_order_relaxed);
        if (!(value & SlotLock) &&
            state.compare_exchange_weak(value, value | SlotLock, std::memory_order_acquire, std::memory_order_relaxed))
            break;
        waited = true;
        sched_yield();
    }
    if (waited && count_wait) counters.waits.fetch_add(1, std::memory_order_relaxed);
    return waited;
}
std::size_t FineKept(SparseRange& range, std::size_t slot) noexcept {
    return range.fine[slot] & 0xff;
}
std::uint8_t SlotState(SparseRange& range, std::size_t slot) noexcept {
    return range.state[slot].load(std::memory_order_acquire) & SlotMask;
}
// Sets the state and lets the slot go.
void UnlockSlot(SparseRange& range, std::size_t slot, std::uint8_t to) noexcept {
    range.owned[slot].store(Mapped(to), std::memory_order_release);
    range.state[slot].store(to, std::memory_order_release);
}
std::uintptr_t SlotAddress(SparseRange& range, std::size_t slot) noexcept {
    return range.begin.load(std::memory_order_relaxed) + slot * SparseSlot;
}

// ---- Blocks: the pool, a reserve for when the pool says no, and shared blocks' references ----
// 256 MiB (was 32): the guest's faults (a slot written first, a packed slot touched again) take a block
// from here when the pool has none. Graphics can wait and give memory back when it runs short; a fault
// in the guest cannot: OOT's Hyrule Field load on the Pro found the pool empty for a packed slot
// ("EDEN_CRAM declined ... state 5 found no block") once 32 MiB had gone. The scanner refills it.
constexpr std::size_t ReserveBlocks = 128;
std::int64_t reserve[ReserveBlocks];
std::atomic<std::size_t> reserve_count{0};
std::atomic_flag reserve_lock = ATOMIC_FLAG_INIT;
constexpr std::size_t SharedBlocks = 4096;
std::int64_t shared_block[SharedBlocks];
int shared_refs[SharedBlocks];
std::atomic_flag shared_lock = ATOMIC_FLAG_INIT;

void Spin(std::atomic_flag& flag) noexcept {
    while (flag.test_and_set(std::memory_order_acquire)) sched_yield();
}

#ifdef PS5_NATIVE
// A zeroed block from the reserve (the pool refused one).
bool ReserveBlock(std::int64_t* physical) noexcept {
    Spin(reserve_lock);
    const std::size_t count = reserve_count.load(std::memory_order_relaxed);
    const bool ok = count > 0;
    if (ok) {
        *physical = reserve[count - 1];
        reserve_count.store(count - 1, std::memory_order_relaxed);
    }
    reserve_lock.clear(std::memory_order_release);
    if (ok) counters.reserve_used.fetch_add(1, std::memory_order_relaxed);
    return ok;
}
void FillReserve() noexcept {
    for (;;) {
        Spin(reserve_lock);
        const bool full = reserve_count.load(std::memory_order_relaxed) >= ReserveBlocks;
        reserve_lock.clear(std::memory_order_release);
        if (full) return;
        std::int64_t physical = -1;
        if (!ZeroedBlock(&physical)) return;
        Spin(reserve_lock);
        const std::size_t count = reserve_count.load(std::memory_order_relaxed);
        if (count < ReserveBlocks) {
            reserve[count] = physical;
            reserve_count.store(count + 1, std::memory_order_relaxed);
            physical = -1;
        }
        reserve_lock.clear(std::memory_order_release);
        if (physical >= 0 && sceKernelReleaseDirectMemory(physical, SparseSlot) != 0) std::abort();
    }
}
void EmptyReserve() noexcept {
    Spin(reserve_lock);
    for (std::size_t i = 0; i < reserve_count.load(std::memory_order_relaxed); ++i)
        if (sceKernelReleaseDirectMemory(reserve[i], SparseSlot) != 0) std::abort();
    reserve_count.store(0, std::memory_order_relaxed);
    reserve_lock.clear(std::memory_order_release);
}
#endif

// One more slot shows `physical`; the first one makes it a shared block. False when the table is full.
bool ShareBlock(std::int64_t physical) noexcept {
    Spin(shared_lock);
    std::size_t free_entry = SharedBlocks;
    bool done = false;
    for (std::size_t i = 0; i < SharedBlocks && !done; ++i) {
        if (shared_refs[i] > 0 && shared_block[i] == physical) {
            ++shared_refs[i];
            done = true;
        } else if (shared_refs[i] == 0 && free_entry == SharedBlocks) {
            free_entry = i;
        }
    }
    if (!done && free_entry < SharedBlocks) {
        shared_block[free_entry] = physical;
        shared_refs[free_entry] = 1;
        done = true;
    }
    shared_lock.clear(std::memory_order_release);
    return done;
}
// One slot less shows `physical`: the references left (0: the block is no longer shown).
int UnshareBlock(std::int64_t physical) noexcept {
    Spin(shared_lock);
    int left = 0;
    for (std::size_t i = 0; i < SharedBlocks; ++i)
        if (shared_refs[i] > 0 && shared_block[i] == physical) {
            left = --shared_refs[i];
            break;
        }
    shared_lock.clear(std::memory_order_release);
    return left;
}

// ---- The changes of a slot (the slot is locked) ----

// Shown or any mapping -> Zero: the slot is only reserved again.
void VoidSlot(std::uintptr_t at) noexcept {
#ifdef PS5_NATIVE
    void* address = reinterpret_cast<void*>(at);
    if (sceKernelReserveVirtualRange(&address, SparseSlot, MAP_FIXED, SparseSlot) == 0 &&
        address == reinterpret_cast<void*>(at))
        return;
    address = reinterpret_cast<void*>(at);
    if (munmap(address, SparseSlot) != 0 || sceKernelReserveVirtualRange(&address, SparseSlot, MAP_FIXED, SparseSlot) != 0 ||
        address != reinterpret_cast<void*>(at))
        std::abort();
#else
    if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_NONE) != 0) std::abort();
#endif
}

// Zero -> Shown: the zero block, read-only.
bool ShowZero(std::uintptr_t at) noexcept {
#ifdef PS5_NATIVE
    return MapSlot(at, PROT_READ, zero_block);
#else
    return mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ) == 0;
#endif
}

// Zero -> Own: a zeroed block of its own, read-write.
bool PromoteZero(SparseRange& range, std::size_t slot) noexcept {
    const std::uintptr_t at = SlotAddress(range, slot);
#ifdef PS5_NATIVE
    std::int64_t physical = -1;
    if (!ZeroedBlock(&physical) && !ReserveBlock(&physical)) return false;
    if (!MapSlot(at, PROT_READ | PROT_WRITE, physical)) {
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        return false;
    }
    range.physical[slot] = physical;
#else
    if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ | PROT_WRITE) != 0) return false;
    range.physical[slot] = 0;
#endif
    guest_committed.fetch_add(SparseSlot, std::memory_order_relaxed);
    return true;
}

// Shared -> Own: the last slot to show a block takes it back read-write; the others copy it.
bool PromoteShared(SparseRange& range, std::size_t slot) noexcept {
    const std::uintptr_t at = SlotAddress(range, slot);
#ifdef PS5_NATIVE
    const std::int64_t shared = range.physical[slot];
    Spin(shared_lock);
    bool last = false;
    for (std::size_t i = 0; i < SharedBlocks; ++i)
        if (shared_refs[i] > 0 && shared_block[i] == shared) {
            last = shared_refs[i] == 1;
            if (last) shared_refs[i] = 0;
            break;
        }
    shared_lock.clear(std::memory_order_release);
    if (last) return mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ | PROT_WRITE) == 0;
    std::int64_t physical = -1;
    if (eden_direct_low(SparseSlot, SparseSlot, 12, &physical) != 0 && !ReserveBlock(&physical)) return false;
    void* view = reinterpret_cast<void*>(cpu_mapping_hint);
    if (sceKernelMapDirectMemory(&view, SparseSlot, PROT_READ | PROT_WRITE, 0, physical, SparseSlot) != 0) {
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        return false;
    }
    std::memcpy(view, reinterpret_cast<const void*>(at), SparseSlot);
    if (munmap(view, SparseSlot) != 0) std::abort();
    if (!MapSlot(at, PROT_READ | PROT_WRITE, physical)) {
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        return false;
    }
    range.physical[slot] = physical;
    if (UnshareBlock(shared) == 0 && sceKernelReleaseDirectMemory(shared, SparseSlot) != 0) std::abort();
    guest_committed.fetch_add(SparseSlot, std::memory_order_relaxed);
    counters.copies.fetch_add(1, std::memory_order_relaxed);
    return true;
#else
    (void)range;
    (void)slot;
    (void)at;
    return false; // the host never shares
#endif
}

// ---- Cold and Packed (the slot is locked) ----
std::uint64_t FullHash(std::uintptr_t at, std::size_t bytes) noexcept;
std::atomic<std::size_t> packed_slots{0}, packed_bytes{0};
// A packed copy given up in the fault handler is freed by the scanner (no free() in a signal handler).
constexpr std::size_t RetireEntries = 4096;
std::atomic<void*> retired[RetireEntries];
void DrainRetired() noexcept {
    for (auto& entry : retired)
        if (void* data = entry.exchange(nullptr, std::memory_order_acquire)) std::free(data);
}
// The packed copies' store: 2 MiB blocks filled one after another. When the open block has no room
// for a copy, the block of the slot being packed becomes the next one (its bytes are in the scratch
// copy by then), so packing never needs memory: copies taken from the heap needed it to grow by a
// 32 MiB piece when the pool was short, and the render thread waited there to pack for an image
// (1.6 s, OOT's Hyrule Field on the Pro, run fritchii-42). A block goes back to the pool with its
// last copy; that may happen in the fault handler (system calls only, no allocation).
constexpr std::size_t PackSlabs = 8192; // 16 GiB of blocks
constexpr std::uint32_t SlabOpen = 1u << 31;
struct PackSlab {
    std::atomic<bool> used{false};
    std::atomic<std::uint32_t> live{0}; // copies in it, plus SlabOpen while copies go in
    std::atomic<std::uint32_t> bytes{0}; // the copies' bytes (64-byte steps)
    char* cpu = nullptr;
    [[maybe_unused]] std::int64_t physical = -1;
};
PackSlab pack_slabs[PackSlabs];
std::atomic_flag pack_store_lock = ATOMIC_FLAG_INIT;
std::uint32_t open_slab = UINT32_MAX; // under pack_store_lock
std::size_t open_used = 0;
std::atomic<std::size_t> pack_store_slabs{0};
void ReleaseSlab(std::uint32_t index) noexcept {
    PackSlab& slab = pack_slabs[index];
    if (munmap(slab.cpu, SparseSlot) != 0) std::abort();
#ifdef PS5_NATIVE
    if (sceKernelReleaseDirectMemory(slab.physical, SparseSlot) != 0) std::abort();
#endif
    slab.cpu = nullptr;
    slab.physical = -1;
    pack_store_slabs.fetch_sub(1, std::memory_order_relaxed);
    slab.used.store(false, std::memory_order_release);
}
constexpr std::uint32_t CopyBytes(std::uint32_t size) noexcept { return (size + 63) & ~63u; }
void DropCopy(std::uint32_t index, std::uint32_t size) noexcept {
    pack_slabs[index].bytes.fetch_sub(CopyBytes(size), std::memory_order_relaxed);
    if (pack_slabs[index].live.fetch_sub(1, std::memory_order_acq_rel) == 1) ReleaseSlab(index);
}
// Under pack_store_lock: `view` (a block's bytes at an address of the store's) is the open block now,
// and the one before is closed (and goes back if it has no copies left).
std::uint32_t OpenSlab(void* view, [[maybe_unused]] std::int64_t physical) noexcept {
    std::uint32_t slab = UINT32_MAX;
    for (std::uint32_t i = 0; i < PackSlabs && slab == UINT32_MAX; ++i)
        if (!pack_slabs[i].used.load(std::memory_order_acquire)) slab = i;
    if (slab == UINT32_MAX) return slab;
    PackSlab& fresh = pack_slabs[slab];
    fresh.used.store(true, std::memory_order_relaxed);
    fresh.cpu = static_cast<char*>(view);
    fresh.physical = physical;
    fresh.bytes.store(0, std::memory_order_relaxed);
    fresh.live.store(SlabOpen, std::memory_order_release);
    pack_store_slabs.fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t closed = open_slab;
    open_slab = slab;
    open_used = 0;
    if (closed != UINT32_MAX && pack_slabs[closed].live.fetch_sub(SlabOpen, std::memory_order_acq_rel) == SlabOpen)
        ReleaseSlab(closed);
    return slab;
}
// Seconds on the steady clock (pack_after's unit).
std::uint32_t NowSeconds() noexcept {
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
// The time from which the slot may be made Cold again.
void PackWait(SparseRange& range, std::size_t slot, std::uint32_t seconds) noexcept {
    if (range.pack_after) range.pack_after[slot] = NowSeconds() + seconds;
}

// Own -> Cold: the block stays, nothing may touch it (its window aliases go too).
bool MakeCold(SparseRange& range, std::size_t slot) noexcept {
    if (range.guest) DropSlotAliases(range, slot);
    return mprotect(reinterpret_cast<void*>(SlotAddress(range, slot)), SparseSlot, PROT_NONE) == 0;
}
// Cold -> Own: touched again.
bool Warm(SparseRange& range, std::size_t slot) noexcept {
    return mprotect(reinterpret_cast<void*>(SlotAddress(range, slot)), SparseSlot, PROT_READ | PROT_WRITE) == 0;
}
// Cold -> Packed: the bytes compressed, the block back to the pool. False (the slot is still Cold, with
// its block) when it does not compress to 3/4 or there is no memory for the copy.
// Set while this thread packs: the heap packs from its wait for memory (headless/heap_arenas.inc), and a
// Pack whose copy's allocation waits there must not pack again (its slot's lock is held, its scratch
// buffer not yet copied out: the game hung on that lock, run fritchii-37).
thread_local bool in_pack = false;
bool PackLocked(SparseRange& range, std::size_t slot) noexcept;
bool Pack(SparseRange& range, std::size_t slot) noexcept {
    in_pack = true;
    const bool packed = PackLocked(range, slot);
    in_pack = false;
    return packed;
}
bool PackLocked(SparseRange& range, std::size_t slot) noexcept {
    if (!range.packed) return false;
    const std::uintptr_t at = SlotAddress(range, slot);
    if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ) != 0) return false;
    // A window alias still recorded could write the block after it goes back to the pool.
    if (range.aliases)
        for (unsigned i = 0; i < AliasesPerSlot; ++i)
            if (range.aliases[slot * AliasesPerSlot + i] != 0) {
                if (counters.pack_aliased.fetch_add(1, std::memory_order_relaxed) < 8) {
                    char line[96];
                    const int length = std::snprintf(line, sizeof(line), "EDEN_CRAM pack: slot %zu still has a window alias\n", slot);
                    if (length > 0) Report(line);
                }
                if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_NONE) != 0) std::abort();
                return false;
            }
    const std::uint64_t hash = FullHash(at, SparseSlot);
    // Compressed into this thread's scratch buffer, then copied to a block of the exact size: a 2 MiB
    // request per pack found no 2 MiB hole in the fragmented heap and grew it by a 32 MiB piece each
    // time, 1.15 GB in OOT's Hyrule Field on the Pro (run fritchii-28), more than packing gave back.
    const int bound = LZ4_compressBound(static_cast<int>(SparseSlot));
    static thread_local std::unique_ptr<char[]> scratch;
    if (!scratch) scratch.reset(new (std::nothrow) char[static_cast<std::size_t>(bound)]);
    const int size = scratch ? LZ4_compress_fast(reinterpret_cast<const char*>(at), scratch.get(), static_cast<int>(SparseSlot), bound, 2) : 0;
    if (size <= 0 || static_cast<std::size_t>(size) > SparseSlot / 4 * 3) {
        if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_NONE) != 0) std::abort();
        return false;
    }
    if (FullHash(at, SparseSlot) != hash) {
        // Something wrote the slot while it was only readable here: a mapping the service does not know.
        if (counters.pack_changed.fetch_add(1, std::memory_order_relaxed) < 8) {
            char line[96];
            const int length = std::snprintf(line, sizeof(line), "EDEN_CRAM pack: slot %zu changed while packing\n", slot);
            if (length > 0) Report(line);
        }
        if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_NONE) != 0) std::abort();
        return false;
    }
    const std::size_t need = (static_cast<std::size_t>(size) + 63) & ~std::size_t{63};
    Spin(pack_store_lock);
    std::uint32_t slab = UINT32_MAX;
    std::size_t offset = 0;
    bool slot_is_slab = false;
    if (open_slab != UINT32_MAX && open_used + need <= SparseSlot) {
        slab = open_slab;
        offset = open_used;
        open_used += need;
        pack_slabs[slab].live.fetch_add(1, std::memory_order_relaxed);
    } else {
        void* view = nullptr;
#ifdef PS5_NATIVE
        // The slot's block, seen at an address of the store's (the slot still shows it read-only).
        view = reinterpret_cast<void*>(cpu_mapping_hint);
        if (sceKernelMapDirectMemory(&view, SparseSlot, PROT_READ | PROT_WRITE, 0, range.physical[slot], SparseSlot) != 0)
            view = nullptr;
        const std::int64_t physical = range.physical[slot];
#else
        view = mmap(nullptr, SparseSlot, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (view == MAP_FAILED) view = nullptr;
        const std::int64_t physical = -1;
#endif
        if (view != nullptr) slab = OpenSlab(view, physical);
        if (slab == UINT32_MAX) {
            if (view != nullptr) (void)munmap(view, SparseSlot);
            pack_store_lock.clear(std::memory_order_release);
            if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_NONE) != 0) std::abort();
            return false;
        }
#ifdef PS5_NATIVE
        slot_is_slab = true;
#endif
        open_used = need;
        pack_slabs[slab].live.fetch_add(1, std::memory_order_relaxed);
    }
    pack_slabs[slab].bytes.fetch_add(static_cast<std::uint32_t>(need), std::memory_order_relaxed);
    char* const data = pack_slabs[slab].cpu + offset;
    pack_store_lock.clear(std::memory_order_release);
#ifdef PS5_NATIVE
    VoidSlot(at);
    if (!slot_is_slab && sceKernelReleaseDirectMemory(range.physical[slot], SparseSlot) != 0) std::abort();
#else
    (void)slot_is_slab;
    if (madvise(reinterpret_cast<void*>(at), SparseSlot, MADV_DONTNEED) != 0) std::abort();
    VoidSlot(at);
#endif
    // After the slot stops showing the block: when it is the store's now, the copy goes over its bytes.
    std::memcpy(data, scratch.get(), static_cast<std::size_t>(size));
    range.physical[slot] = -1;
    range.packed[slot] = {data, static_cast<std::uint32_t>(size), hash, slab};
    guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
    packed_slots.fetch_add(1, std::memory_order_relaxed);
    packed_bytes.fetch_add(static_cast<std::size_t>(size), std::memory_order_relaxed);
    return true;
}
// Packed -> Own: a new block with the bytes back (the fault handler too: no allocation here). The
// bytes go into the block through a view of its own first, and the slot shows it only then: a slot
// writable before its bytes are back would lose a write another thread makes meanwhile (the
// decompression would overwrite it), which a check of the bytes found on the host.
bool Unpack(SparseRange& range, std::size_t slot) noexcept {
    const std::uintptr_t at = SlotAddress(range, slot);
    auto& packed = range.packed[slot];
    const auto expand = [&packed](void* into) {
        if (LZ4_decompress_safe(static_cast<const char*>(packed.data), static_cast<char*>(into), static_cast<int>(packed.size),
                                static_cast<int>(SparseSlot)) != static_cast<int>(SparseSlot))
            std::abort(); // the copy is the slot's only bytes
        if (FullHash(reinterpret_cast<std::uintptr_t>(into), SparseSlot) != packed.hash)
            counters.unpack_wrong.fetch_add(1, std::memory_order_relaxed);
    };
#ifdef PS5_NATIVE
    std::int64_t physical = -1;
    if (eden_direct_low(SparseSlot, SparseSlot, 12, &physical) != 0 && !ReserveBlock(&physical)) return false;
    void* view = reinterpret_cast<void*>(cpu_mapping_hint);
    if (sceKernelMapDirectMemory(&view, SparseSlot, PROT_READ | PROT_WRITE, 0, physical, SparseSlot) != 0) {
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        return false;
    }
    expand(view);
    if (munmap(view, SparseSlot) != 0) std::abort();
    if (!MapSlot(at, PROT_READ | PROT_WRITE, physical)) {
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        return false;
    }
    range.physical[slot] = physical;
#else
    void* fresh = mmap(nullptr, SparseSlot, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fresh == MAP_FAILED) return false;
    expand(fresh);
    if (mremap(fresh, SparseSlot, SparseSlot, MREMAP_MAYMOVE | MREMAP_FIXED, reinterpret_cast<void*>(at)) == MAP_FAILED) {
        (void)munmap(fresh, SparseSlot);
        return false;
    }
    range.physical[slot] = 0;
#endif
    DropCopy(packed.slab, packed.size);
    packed_slots.fetch_sub(1, std::memory_order_relaxed);
    packed_bytes.fetch_sub(packed.size, std::memory_order_relaxed);
    packed = {nullptr, 0, 0, 0};
    guest_committed.fetch_add(SparseSlot, std::memory_order_relaxed);
    counters.unpacks.fetch_add(1, std::memory_order_relaxed);
    PackWait(range, slot, UnpackWait);
    return true;
}
// A Packed slot that goes back (cleared or freed whole): its copy goes. Not in the fault handler.
void DropPacked(SparseRange& range, std::size_t slot) noexcept {
    auto& packed = range.packed[slot];
    packed_slots.fetch_sub(1, std::memory_order_relaxed);
    packed_bytes.fetch_sub(packed.size, std::memory_order_relaxed);
    DropCopy(packed.slab, packed.size);
    packed = {nullptr, 0, 0, 0};
}

// Own, Shared or Fine -> Zero: the slot shows the zero block again and its memory goes back.
void ReturnSlot(SparseRange& range, std::size_t slot) noexcept {
    if (range.guest) DropSlotAliases(range, slot);
    const std::uintptr_t at = SlotAddress(range, slot);
    const std::uint8_t state = SlotState(range, slot);
    if (state == SlotShown) {
        VoidSlot(at);
        return;
    }
    if (state == SlotPacked) { // nothing mapped, nothing in the pool: only the copy
        DropPacked(range, slot);
        return;
    }
    // Cold gives its block back as Own does (below).
    if (state == SlotFine) {
        const std::size_t kept = FineKept(range, slot);
#ifdef PS5_NATIVE
        VoidSlot(at);
        if (kept != 0 && sceKernelReleaseDirectMemory(range.physical[slot], kept * FinePage) != 0) std::abort();
#else
        if (madvise(reinterpret_cast<void*>(at), SparseSlot, MADV_DONTNEED) != 0) std::abort();
        VoidSlot(at);
#endif
        guest_committed.fetch_sub(kept * FinePage, std::memory_order_relaxed);
        range.fine[slot] &= 0xf00;
        range.physical[slot] = -1;
        return;
    }
#ifdef PS5_NATIVE
    VoidSlot(at);
    if (state == SlotShared) {
        if (UnshareBlock(range.physical[slot]) == 0) {
            if (sceKernelReleaseDirectMemory(range.physical[slot], SparseSlot) != 0) std::abort();
            guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
        }
    } else {
        if (sceKernelReleaseDirectMemory(range.physical[slot], SparseSlot) != 0) std::abort();
        guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
    }
#else
    (void)state;
    if (madvise(reinterpret_cast<void*>(at), SparseSlot, MADV_DONTNEED) != 0) std::abort();
    VoidSlot(at);
    guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
#endif
    range.physical[slot] = -1;
}

bool AllZero(std::uintptr_t at) noexcept {
    const auto* word = reinterpret_cast<const volatile std::uint64_t*>(at);
    for (std::size_t i = 0; i < SparseSlot / 8; i += 8) {
        if (word[i] | word[i + 1] | word[i + 2] | word[i + 3] | word[i + 4] | word[i + 5] | word[i + 6] | word[i + 7])
            return false;
    }
    return true;
}

bool PageZero(std::uintptr_t at) noexcept {
    const auto* word = reinterpret_cast<const volatile std::uint64_t*>(at);
    for (std::size_t i = 0; i < FinePage / 8; i += 8)
        if (word[i] | word[i + 1] | word[i + 2] | word[i + 3] | word[i + 4] | word[i + 5] | word[i + 6] | word[i + 7])
            return false;
    return true;
}

// Fine -> Own: a written Fine slot becomes one block of 2 MiB again (a copy of what it shows), and
// stays whole for the rest of the session.
bool PromoteFine(SparseRange& range, std::size_t slot) noexcept {
    const std::uintptr_t at = SlotAddress(range, slot);
    const std::size_t kept = FineKept(range, slot);
#ifdef PS5_NATIVE
    std::int64_t physical = -1;
    if (eden_direct_low(SparseSlot, SparseSlot, 12, &physical) != 0 && !ReserveBlock(&physical)) return false;
    void* view = reinterpret_cast<void*>(cpu_mapping_hint);
    if (sceKernelMapDirectMemory(&view, SparseSlot, PROT_READ | PROT_WRITE, 0, physical, SparseSlot) != 0) {
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        return false;
    }
    std::memcpy(view, reinterpret_cast<const void*>(at), SparseSlot);
    if (munmap(view, SparseSlot) != 0) std::abort();
    if (!MapSlot(at, PROT_READ | PROT_WRITE, physical)) {
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        return false;
    }
    if (kept != 0 && sceKernelReleaseDirectMemory(range.physical[slot], kept * FinePage) != 0) std::abort();
    range.physical[slot] = physical;
#else
    if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ | PROT_WRITE) != 0) return false;
#endif
    guest_committed.fetch_add(SparseSlot - kept * FinePage, std::memory_order_relaxed);
    const unsigned times = std::min<unsigned>(((range.fine[slot] >> 8) & 0xf) + 1, FineBackoffMax);
    range.fine[slot] = static_cast<std::uint16_t>(times << 8);
    range.fine_after[slot] = static_cast<std::uint32_t>(counters.passes.load(std::memory_order_relaxed)) + (1u << times);
    counters.unfined.fetch_add(1, std::memory_order_relaxed);
    return true;
}

#ifdef PS5_NATIVE
bool MapRun(std::uintptr_t at, std::size_t bytes, std::int64_t physical) noexcept {
    void* address = reinterpret_cast<void*>(at);
    return sceKernelMapDirectMemory(&address, bytes, PROT_READ, MAP_FIXED, physical, FinePage) == 0 &&
           address == reinterpret_cast<void*>(at);
}
#endif

// Own -> Fine (the slot is locked and fenced, and has `zero` marked in a mask of its pages): its other
// pages go into a compact block, and the slot is mapped again page run by page run, read-only.
// The 2 MiB mapping is taken away whole first (a large mapping is never split); a thread that
// touches the slot meanwhile faults, waits for the lock and repeats its access.
bool MakeFine(SparseRange& range, std::size_t slot, const bool (&zero)[FinePages], std::size_t kept) noexcept {
    const std::uintptr_t at = SlotAddress(range, slot);
#ifdef PS5_NATIVE
    std::int64_t compact = -1;
    if (eden_direct_low(kept * FinePage, FinePage, 12, &compact) != 0) return false;
    void* view = reinterpret_cast<void*>(cpu_mapping_hint);
    if (sceKernelMapDirectMemory(&view, kept * FinePage, PROT_READ | PROT_WRITE, 0, compact, FinePage) != 0) {
        (void)sceKernelReleaseDirectMemory(compact, kept * FinePage);
        return false;
    }
    std::size_t next = 0;
    for (std::size_t page = 0; page < FinePages; ++page)
        if (!zero[page]) {
            std::memcpy(static_cast<std::uint8_t*>(view) + next * FinePage,
                        reinterpret_cast<const void*>(at + page * FinePage), FinePage);
            ++next;
        }
    if (munmap(view, kept * FinePage) != 0) std::abort();
    const std::int64_t whole = range.physical[slot];
    if (munmap(reinterpret_cast<void*>(at), SparseSlot) != 0) std::abort();
    bool ok = true;
    next = 0;
    for (std::size_t page = 0; page < FinePages && ok;) {
        std::size_t end = page + 1;
        while (end < FinePages && zero[end] == zero[page]) ++end;
        const std::size_t bytes = (end - page) * FinePage;
        ok = MapRun(at + page * FinePage, bytes, zero[page] ? zero_block : compact + std::int64_t(next * FinePage));
        if (!zero[page]) next += end - page;
        page = end;
    }
    if (!ok) {
        // The slot shows its block again, as it was (fenced: read-only until the caller unfences).
        if (!MapSlot(at, PROT_READ, whole)) std::abort();
        (void)sceKernelReleaseDirectMemory(compact, kept * FinePage);
        return false;
    }
    if (sceKernelReleaseDirectMemory(whole, SparseSlot) != 0) std::abort();
    range.physical[slot] = compact;
#else
    for (std::size_t page = 0; page < FinePages; ++page)
        if (zero[page] && madvise(reinterpret_cast<void*>(at + page * FinePage), FinePage, MADV_DONTNEED) != 0)
            std::abort();
#endif
    guest_committed.fetch_sub(SparseSlot - kept * FinePage, std::memory_order_relaxed);
    range.fine[slot] = static_cast<std::uint16_t>((range.fine[slot] & 0xf00) | kept);
    if (range.aliases) {
        std::uint64_t* bits = FineZero(range, slot);
        bits[0] = bits[1] = 0;
        for (std::size_t page = 0; page < FinePages; ++page)
            if (zero[page]) bits[page / 64] |= std::uint64_t{1} << (page % 64);
    }
    counters.fined.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// CommitSparsePage for a range with slot states.
bool ServiceCommit(SparseRange& range, std::size_t slot) noexcept {
    LockSlot(range, slot);
    const std::uint8_t state = SlotState(range, slot);
    if (range.guest && state != SlotOwn) DropSlotAliases(range, slot);  // read-only window pieces
    const bool ok = Empty(state) ? PromoteZero(range, slot) : state == SlotFine ? PromoteFine(range, slot)
                  : state == SlotCold ? Warm(range, slot) : state == SlotPacked ? Unpack(range, slot) : true;
    UnlockSlot(range, slot, ok && (Empty(state) || state == SlotFine || state == SlotCold || state == SlotPacked) ? SlotOwn : state);
    return ok;
}

// ---- The fault ----
SparseRange* WriteRangeOf(std::uintptr_t address) noexcept {
    for (SparseRange* range : {service_range.load(std::memory_order_acquire), probe_range.load(std::memory_order_acquire)}) {
        if (range == nullptr || range->state == nullptr) continue;
        const auto begin = range->begin.load(std::memory_order_acquire);
        if (begin != 0 && address >= begin && address < range->end) return range;
    }
    return nullptr;
}
std::atomic<std::uintptr_t> probe_expected{0}; // the self-test's write, whatever si_addr says
std::atomic<bool> probe_mismatch{false};
} // namespace

// A write fault at `address`: true when it was a write to a slot of the guest's memory that is
// now writable (the faulting instruction repeats), false for anything else.
bool HandleGuestWriteFault(void* fault) noexcept {
    auto address = reinterpret_cast<std::uintptr_t>(fault);
    if (const auto expected = probe_expected.load(std::memory_order_acquire); expected != 0 && address != expected) {
        probe_mismatch.store(true, std::memory_order_relaxed);
        address = expected;
    }
    SparseRange* const range = WriteRangeOf(address);
    if (range == nullptr) return false;
    // The same address faulting again and again on a slot that is already its own is not a slot
    // to promote but a real fault. Faults that move the slot on (Zero, Shown, Shared, Fine) are
    // not counted: a game that frees and maps the same memory again writes the same address
    // after every free, and each time the slot starts from Zero (Ocarina of Time in gameplay).
    static thread_local std::uintptr_t last = 0;
    static thread_local unsigned repeats = 0;
    counters.faults.fetch_add(1, std::memory_order_relaxed);
    const std::size_t slot = (address - range->begin.load(std::memory_order_relaxed)) / SparseSlot;
    const bool waited = LockSlot(*range, slot, true);
    const std::uint8_t state = SlotState(*range, slot);
    repeats = address == last && state == SlotOwn ? repeats + 1 : 0;
    last = address;
    if (repeats > 16) {
        repeats = 0;
        UnlockSlot(*range, slot, state);
        // Declined: why, for the crash's log (the same address 17 times on an Own slot).
        char line[160];
        const int length = std::snprintf(line, sizeof(line),
            "EDEN_CRAM declined: address %#lx slot %zu state Own faulted 17 times (physical %#llx)\n",
            static_cast<unsigned long>(address), slot, static_cast<unsigned long long>(range->physical[slot]));
        if (length > 0) Note(line);
        return false;
    }
    if (waited) {
        // Someone changed the slot meanwhile (or is mapping it again): the access may now succeed,
        // and a read must never make a Fine slot whole. It faults again if it was a write.
        UnlockSlot(*range, slot, state);
        return true;
    }
    if (state == SlotZero) {
        // Nothing mapped: the zero block shows, and a write faults again to take a block.
        const bool shown = ShowZero(SlotAddress(*range, slot));
        UnlockSlot(*range, slot, shown ? SlotShown : SlotZero);
        if (shown) counters.shows.fetch_add(1, std::memory_order_relaxed);
        return shown;
    }
    bool ok = true;
    // The window's read-only pieces of this slot would keep showing the old block.
    if (range->guest && state != SlotOwn) DropSlotAliases(*range, slot);
    if (state == SlotFine) {
        ok = PromoteFine(*range, slot);
    } else if (state == SlotCold) {
        // Touched within the pass that made it Cold: its own again, left alone for a while.
        ok = Warm(*range, slot);
        if (ok) {
            counters.warms.fetch_add(1, std::memory_order_relaxed);
            PackWait(*range, slot, WarmWait);
        }
    } else if (state == SlotPacked) {
        ok = Unpack(*range, slot); // any access: the bytes come back
    } else if (state == SlotShown) {
        ok = PromoteZero(*range, slot);
        if (ok) counters.promotions.fetch_add(1, std::memory_order_relaxed);
    } else if (state == SlotShared) {
        ok = PromoteShared(*range, slot);
    }
    UnlockSlot(*range, slot, ok ? SlotOwn : state);
    if (!ok) {
        char line[128];
        const int length = std::snprintf(line, sizeof(line), "EDEN_CRAM declined: address %#lx slot %zu state %u found no block\n",
                                         static_cast<unsigned long>(address), slot, unsigned(state));
        if (length > 0) Note(line);
        counters.failures.fetch_add(1, std::memory_order_relaxed);
        Note(state == SlotPacked ? "EDEN_CRAM out of memory: a packed slot of the guest's memory found no block to unpack into\n"
                                 : "EDEN_CRAM out of memory: a written slot of the guest's memory found no block\n");
    }
    return ok;
}

namespace {
struct sigaction service_previous_segv{}, service_previous_bus{};

void ServiceSignal(int signal, siginfo_t* info, void* context) {
    if (HandleGuestWriteFault(info ? info->si_addr : nullptr)) return;
    const struct sigaction& previous = signal == SIGSEGV ? service_previous_segv : service_previous_bus;
    if (previous.sa_flags & SA_SIGINFO) {
        previous.sa_sigaction(signal, info, context);
    } else if (previous.sa_handler == SIG_DFL) {
        std::signal(signal, SIG_DFL); // the faulting instruction repeats and ends the process
    } else if (previous.sa_handler != SIG_IGN) {
        previous.sa_handler(signal);
    }
}

bool InstallServiceSignal() noexcept {
    static std::once_flag once;
    static bool installed = false;
    std::call_once(once, [] {
        struct sigaction action{};
        action.sa_sigaction = ServiceSignal;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        installed = sigaction(SIGSEGV, &action, &service_previous_segv) == 0 &&
                    sigaction(SIGBUS, &action, &service_previous_bus) == 0;
    });
    return installed;
}

// Gives a range slot states (a guest range, or the self-test's).
bool AddStates(SparseRange& range) noexcept {
    const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
    auto* state = new (std::nothrow) std::atomic<std::uint8_t>[slots];
    auto* fine = state ? new (std::nothrow) std::uint16_t[slots]() : nullptr;
    auto* fine_after = fine ? new (std::nothrow) std::uint32_t[slots]() : nullptr;
    // The alias table, then per slot a Fine slot's zero pages (FineZero).
    auto* aliases = fine_after && range.guest ? new (std::nothrow) std::uintptr_t[slots * (AliasesPerSlot + 2)]() : nullptr;
    auto* packed = fine_after ? new (std::nothrow) SparseRange::PackedBytes[slots]() : nullptr;
    auto* pack_after = packed ? new (std::nothrow) std::uint32_t[slots]() : nullptr;
    if (pack_after == nullptr || (range.guest && aliases == nullptr)) {
        delete[] state;
        delete[] fine;
        delete[] fine_after;
        delete[] aliases;
        delete[] packed;
        return false;
    }
    range.packed = packed;
    range.pack_after = pack_after;
    range.aliases = aliases;
    range.fine = fine;
    range.fine_after = fine_after;
    for (std::size_t slot = 0; slot < slots; ++slot)
        state[slot].store(range.owned[slot].load(std::memory_order_relaxed) ? SlotOwn : SlotShown, std::memory_order_relaxed);
    range.state = state;
    return true;
}

// ---- The scanner ----
std::thread scanner;
std::mutex scanner_mutex;
std::condition_variable scanner_wake;
std::atomic<bool> scanner_kicked{false};
std::size_t PackCold(SparseRange& range, std::size_t want, std::size_t max_packs, bool sweep, bool emergency = false) noexcept;
bool scanner_stop = false;

std::uint64_t Mix(std::uint64_t hash, std::uint64_t word) noexcept {
    hash ^= word + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash * 0xff51afd7ed558ccdull;
}
std::uint64_t SampleHash(std::uintptr_t at) noexcept {
    const auto* word = reinterpret_cast<const volatile std::uint64_t*>(at);
    std::uint64_t hash = 0;
    for (std::size_t i = 0; i < SparseSlot / 8; i += SparseSlot / 8 / 64) hash = Mix(hash, word[i] ^ word[i + 1]);
    return hash;
}
std::uint64_t FullHash(std::uintptr_t at, std::size_t bytes) noexcept {
    const auto* word = reinterpret_cast<const volatile std::uint64_t*>(at);
    std::uint64_t hash = 0;
    for (std::size_t i = 0; i < bytes / 8; ++i) hash = Mix(hash, word[i]);
    return hash;
}

void Fence(std::uintptr_t at) noexcept {
    if (auto* range = guest_range.load(std::memory_order_acquire); range && at >= range->begin.load(std::memory_order_relaxed) &&
        at < range->end)
        DropSlotAliases(*range, (at - range->begin.load(std::memory_order_relaxed)) / SparseSlot);
    if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ) != 0) std::abort();
}
void Unfence(std::uintptr_t at) noexcept {
    if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ | PROT_WRITE) != 0) std::abort();
}

// An Own slot that holds only zeroes goes back. Fenced while it is looked at again.
bool ScanZero(SparseRange& range, std::size_t slot) noexcept {
    LockSlot(range, slot);
    if (SlotState(range, slot) != SlotOwn) {
        UnlockSlot(range, slot, SlotState(range, slot));
        return false;
    }
    const std::uintptr_t at = SlotAddress(range, slot);
    Fence(at);
    const bool zero = AllZero(at);
    if (zero) ReturnSlot(range, slot);
    else Unfence(at);
    UnlockSlot(range, slot, zero ? SlotZero : SlotOwn);
    return zero;
}

// An Own slot with many pages of zeroes becomes Fine. A racy count first, then the count again fenced.
bool ScanFine(SparseRange& range, std::size_t slot) noexcept {
    const std::uintptr_t at = SlotAddress(range, slot);
    if (counters.passes.load(std::memory_order_relaxed) < range.fine_after[slot]) return false;
    std::size_t zeros = 0;
    for (std::size_t page = 0; page < FinePages; ++page) zeros += PageZero(at + page * FinePage);
    if (zeros < FineMinZeroPages) return false;
    LockSlot(range, slot);
    if (SlotState(range, slot) != SlotOwn) {
        UnlockSlot(range, slot, SlotState(range, slot));
        return false;
    }
    Fence(at);
    bool zero[FinePages];
    zeros = 0;
    for (std::size_t page = 0; page < FinePages; ++page) zeros += zero[page] = PageZero(at + page * FinePage);
    bool fined = false;
    if (zeros == FinePages) {
        ReturnSlot(range, slot);
        UnlockSlot(range, slot, SlotZero);
        counters.returns_scan.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (zeros >= FineMinZeroPages) {
        fined = MakeFine(range, slot, zero, FinePages - zeros);
        if (!fined) counters.fine_failures.fetch_add(1, std::memory_order_relaxed);
    }
    if (!fined) Unfence(at);
    UnlockSlot(range, slot, fined ? SlotFine : SlotOwn);
    return fined;
}

// Two slots of the same bytes show one block (the console; the host only counts them).
bool ScanShare(SparseRange& range, std::size_t keep, std::size_t other) noexcept {
    const std::size_t first = std::min(keep, other), second = std::max(keep, other);
    LockSlot(range, first);
    LockSlot(range, second);
    const std::uint8_t keep_state = SlotState(range, keep), other_state = SlotState(range, other);
    bool shared = false;
    if ((keep_state == SlotOwn || keep_state == SlotShared) && other_state == SlotOwn) {
        const std::uintptr_t keep_at = SlotAddress(range, keep), other_at = SlotAddress(range, other);
        if (keep_state == SlotOwn) Fence(keep_at);
        Fence(other_at);
        const bool same = std::memcmp(reinterpret_cast<const void*>(keep_at), reinterpret_cast<const void*>(other_at),
                                      SparseSlot) == 0;
#ifdef PS5_NATIVE
        // A block becoming shared counts both slots (the first ShareBlock makes it 1, the second 2); one
        // already shared counts one more. A table that is full shares nothing.
        bool counted = false;
        if (same && ShareBlock(range.physical[keep])) {
            counted = keep_state == SlotShared || ShareBlock(range.physical[keep]);
            if (!counted) (void)UnshareBlock(range.physical[keep]);
        }
        if (counted) {
            if (!MapSlot(other_at, PROT_READ, range.physical[keep])) std::abort();
            if (sceKernelReleaseDirectMemory(range.physical[other], SparseSlot) != 0) std::abort();
            range.physical[other] = range.physical[keep];
            guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
            shared = true;
        }
#else
        (void)same;
#endif
        if (!shared) {
            if (keep_state == SlotOwn) Unfence(keep_at);
            Unfence(other_at);
        }
        if (same) counters.dedup.fetch_add(1, std::memory_order_relaxed);
    }
    UnlockSlot(range, second, second == keep ? (shared ? SlotShared : keep_state) : (shared ? SlotShared : other_state));
    UnlockSlot(range, first, first == keep ? (shared ? SlotShared : keep_state) : (shared ? SlotShared : other_state));
    return shared;
}

// An Own slot nothing touched since the last pass (or that may be looked at again) becomes Cold.
void ScanCold(SparseRange& range, std::size_t slot) noexcept {
    if (range.pack_after && NowSeconds() < range.pack_after[slot]) return;
    LockSlot(range, slot);
    const bool cold = SlotState(range, slot) == SlotOwn && MakeCold(range, slot);
    UnlockSlot(range, slot, cold ? SlotCold : SlotState(range, slot));
    if (cold) counters.colds.fetch_add(1, std::memory_order_relaxed);
}
// A slot still Cold a pass later is packed; one that does not compress is its own again for a while.
void ScanPack(SparseRange& range, std::size_t slot) noexcept {
    LockSlot(range, slot);
    if (SlotState(range, slot) != SlotCold) {
        UnlockSlot(range, slot, SlotState(range, slot));
        return;
    }
    if (Pack(range, slot)) {
        UnlockSlot(range, slot, SlotPacked);
        counters.packs.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!Warm(range, slot)) std::abort(); // a Cold slot left unreachable would lose its bytes' writers
    PackWait(range, slot, RefusedWait);
    UnlockSlot(range, slot, SlotOwn);
    counters.pack_refused.fetch_add(1, std::memory_order_relaxed);
}

void ScanPass(SparseRange& range) noexcept {
    DrainRetired();
    const bool pack = pack_requested.load(std::memory_order_relaxed) && pack_works.load(std::memory_order_relaxed) &&
                      range.packed != nullptr;
    const auto start = std::chrono::steady_clock::now();
    const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
    std::vector<std::pair<std::uint64_t, std::size_t>> samples;
    // Made Cold at the end of the pass: the duplicate search below reads slots, and a read of a
    // Cold slot makes it Own again.
    std::vector<std::size_t> to_cold;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        const std::uint8_t state = SlotState(range, slot);
        if (pack && state == SlotCold) {
            ScanPack(range, slot);
            continue;
        }
        if (state != SlotOwn) continue;
        const std::uintptr_t at = SlotAddress(range, slot);
        if (AllZero(at)) { // a read without the lock: only a hint, checked again fenced
            if (ScanZero(range, slot)) counters.returns_scan.fetch_add(1, std::memory_order_relaxed);
        } else if (fine_requested.load(std::memory_order_relaxed) && ScanFine(range, slot)) {
            // now Fine
        } else {
            if (dedup_requested.load(std::memory_order_relaxed)) samples.emplace_back(SampleHash(at), slot);
            if (pack) to_cold.push_back(slot);
        }
    }
    std::sort(samples.begin(), samples.end());
    for (std::size_t i = 0; i < samples.size();) {
        std::size_t j = i + 1;
        while (j < samples.size() && samples[j].first == samples[i].first) ++j;
        if (j - i > 1) {
            std::vector<std::pair<std::uint64_t, std::size_t>> full;
            for (std::size_t k = i; k < j; ++k)
                full.emplace_back(FullHash(SlotAddress(range, samples[k].second), SparseSlot), samples[k].second);
            std::sort(full.begin(), full.end());
            for (std::size_t a = 0; a < full.size();) {
                std::size_t b = a + 1;
                while (b < full.size() && full[b].first == full[a].first) {
                    (void)ScanShare(range, full[a].second, full[b].second);
                    ++b;
                }
                a = b;
            }
        }
        i = j;
    }
    for (const std::size_t slot : to_cold) ScanCold(range, slot);
    counters.passes.fetch_add(1, std::memory_order_relaxed);
    counters.scan_ns.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
}

void ServiceReport(SparseRange& range, const char* when) noexcept {
    const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
    std::size_t own = 0, shared = 0, fine = 0, fine_kept = 0, shown = 0, cold = 0, packed = 0;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        const auto state = SlotState(range, slot);
        cold += state == SlotCold;
        packed += state == SlotPacked;
        own += state == SlotOwn;
        shown += state == SlotShown;
        shared += state == SlotShared;
        if (state == SlotFine) {
            ++fine;
            fine_kept += FineKept(range, slot);
        }
    }
    // The title's page tables (one CPU table per 2 MiB mapped): what this range leaves the rest.
    int cpu_tables = 0, cpu_free = -1, gpu_tables = 0, gpu_free = -1;
#ifdef PS5_NATIVE
    if (sceKernelGetPageTableStats(&cpu_tables, &cpu_free, &gpu_tables, &gpu_free) != 0) cpu_free = gpu_free = -1;
#endif
    char line[1024];
    const int length = std::snprintf(line, sizeof(line),
        "EDEN_CRAM %s held=%zuMiB own=%zu shared=%zu zero=%zu shown=%zu shows=%llu page_tables_free=%d/%d gpu=%d/%d faults=%llu promotions=%llu copies=%llu waits=%llu "
        "returns_clear=%llu returns_free=%llu returns_scan=%llu dedup=%llu reserve_used=%llu failures=%llu "
        "passes=%llu scan_ms=%llu fine=%zu fine_held=%zuMiB fined=%llu unfined=%llu fine_failures=%llu "
        "cold=%zu packed=%zu packed_held=%zuMiB colds=%llu warms=%llu packs=%llu unpacks=%llu pack_refused=%llu retire_full=%llu pack_store=%zuMiB "
        "sweeps=%llu pack_aliased=%llu pack_changed=%llu unpack_wrong=%llu\n",
        when, guest_committed.load(std::memory_order_relaxed) >> 20, own, shared, slots - own - shared - fine - shown - cold - packed,
        shown, (unsigned long long)counters.shows.load(), cpu_free, cpu_tables, gpu_free, gpu_tables,
        (unsigned long long)counters.faults.load(), (unsigned long long)counters.promotions.load(),
        (unsigned long long)counters.copies.load(), (unsigned long long)counters.waits.load(),
        (unsigned long long)counters.returns_clear.load(), (unsigned long long)counters.returns_free.load(),
        (unsigned long long)counters.returns_scan.load(), (unsigned long long)counters.dedup.load(),
        (unsigned long long)counters.reserve_used.load(), (unsigned long long)counters.failures.load(),
        (unsigned long long)counters.passes.load(), (unsigned long long)(counters.scan_ns.load() / 1000000),
        fine, fine_kept * FinePage >> 20, (unsigned long long)counters.fined.load(),
        (unsigned long long)counters.unfined.load(), (unsigned long long)counters.fine_failures.load(),
        cold, packed, packed_bytes.load(std::memory_order_relaxed) >> 20, (unsigned long long)counters.colds.load(),
        (unsigned long long)counters.warms.load(), (unsigned long long)counters.packs.load(),
        (unsigned long long)counters.unpacks.load(), (unsigned long long)counters.pack_refused.load(),
        (unsigned long long)counters.retire_full.load(), pack_store_slabs.load(std::memory_order_relaxed) * SparseSlot >> 20, (unsigned long long)counters.sweeps.load(),
        (unsigned long long)counters.pack_aliased.load(), (unsigned long long)counters.pack_changed.load(),
        (unsigned long long)counters.unpack_wrong.load());
    if (length > 0) Report(line);
}

// What finer slots would give: 16 KiB pages of the Own slots that hold only zeroes, and those that
// repeat another page (by a full hash). Read once, when the session ends.
void MeasureFinePages(SparseRange& range) noexcept {
    constexpr std::size_t Fine = 16384;
    const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
    std::vector<std::uint64_t> hashes;
    std::size_t pages = 0, zero = 0;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        if (SlotState(range, slot) != SlotOwn) continue;
        for (std::size_t offset = 0; offset < SparseSlot; offset += Fine) {
            const std::uintptr_t at = SlotAddress(range, slot) + offset;
            ++pages;
            const auto* word = reinterpret_cast<const volatile std::uint64_t*>(at);
            bool is_zero = true;
            for (std::size_t i = 0; i < Fine / 8 && is_zero; ++i) is_zero = word[i] == 0;
            if (is_zero) ++zero;
            else hashes.push_back(FullHash(at, Fine));
        }
    }
    std::sort(hashes.begin(), hashes.end());
    std::size_t repeats = 0;
    for (std::size_t i = 1; i < hashes.size(); ++i) repeats += hashes[i] == hashes[i - 1];
    char line[192];
    const int length = std::snprintf(line, sizeof(line),
        "EDEN_CRAM_FINE pages16k=%zu zero=%zu (%zuMiB) repeats=%zu (%zuMiB)\n", pages, zero, zero * Fine >> 20, repeats,
        repeats * Fine >> 20);
    if (length > 0) Report(line);
}

// The store's blocks fill one after another and a copy that goes leaves a hole: 1.4 GB of blocks held
// 0.86 GB of copies (run fritchii-44). Over a quarter more than the copies, the copies of the emptiest
// closed blocks move into the open one (a new block when it is full) and those blocks go back.
void CompactStore(SparseRange& range) noexcept {
    if (range.packed == nullptr) return;
    const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
    for (int round = 0; round < 16; ++round) {
        const std::size_t held = pack_store_slabs.load(std::memory_order_relaxed) * SparseSlot;
        if (held <= packed_bytes.load(std::memory_order_relaxed) / 4 * 5 + (std::size_t{32} << 20)) return;
        std::uint32_t victim = UINT32_MAX, least = static_cast<std::uint32_t>(SparseSlot / 2);
        for (std::uint32_t i = 0; i < PackSlabs; ++i) {
            const PackSlab& slab = pack_slabs[i];
            if (!slab.used.load(std::memory_order_acquire) || (slab.live.load(std::memory_order_relaxed) & SlabOpen)) continue;
            const std::uint32_t bytes = slab.bytes.load(std::memory_order_relaxed);
            if (bytes < least) {
                least = bytes;
                victim = i;
            }
        }
        if (victim == UINT32_MAX) return;
        for (std::size_t slot = 0; slot < slots; ++slot) {
            if (SlotState(range, slot) != SlotPacked || range.packed[slot].slab != victim) continue;
            LockSlot(range, slot);
            auto& packed = range.packed[slot];
            if (SlotState(range, slot) != SlotPacked || packed.slab != victim) {
                UnlockSlot(range, slot, SlotState(range, slot));
                continue;
            }
            const std::uint32_t need = CopyBytes(packed.size);
            Spin(pack_store_lock);
            if (open_slab == UINT32_MAX || open_used + need > SparseSlot) {
                void* view = nullptr;
                std::int64_t physical = -1;
#ifdef PS5_NATIVE
                if (eden_direct_low(SparseSlot, SparseSlot, 12, &physical) == 0) {
                    view = reinterpret_cast<void*>(cpu_mapping_hint);
                    if (sceKernelMapDirectMemory(&view, SparseSlot, PROT_READ | PROT_WRITE, 0, physical, SparseSlot) != 0) {
                        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
                        view = nullptr;
                    }
                }
#else
                view = mmap(nullptr, SparseSlot, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (view == MAP_FAILED) view = nullptr;
#endif
                if (view == nullptr || OpenSlab(view, physical) == UINT32_MAX) {
                    if (view != nullptr) {
                        (void)munmap(view, SparseSlot);
#ifdef PS5_NATIVE
                        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
#endif
                    }
                    pack_store_lock.clear(std::memory_order_release);
                    UnlockSlot(range, slot, SlotPacked);
                    return;
                }
            }
            const std::uint32_t slab = open_slab;
            char* const data = pack_slabs[slab].cpu + open_used;
            open_used += need;
            pack_slabs[slab].live.fetch_add(1, std::memory_order_relaxed);
            pack_slabs[slab].bytes.fetch_add(need, std::memory_order_relaxed);
            pack_store_lock.clear(std::memory_order_release);
            std::memcpy(data, packed.data, packed.size);
            const std::uint32_t old = packed.slab;
            packed.data = data;
            packed.slab = slab;
            DropCopy(old, packed.size);
            UnlockSlot(range, slot, SlotPacked);
        }
    }
}

constexpr unsigned long long ShortOfFree = 5ULL << 29; // 2.5 GiB
void ScannerLoop(SparseRange* range) {
    auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::unique_lock lock{scanner_mutex};
    while (!scanner_stop) {
        // Every 5 s while slots are packed (a slot untouched for 5 s is Cold long enough), else 20 s.
        const bool packing = pack_requested.load(std::memory_order_relaxed) && pack_works.load(std::memory_order_relaxed);
        const auto free_reader = pool_free.load(std::memory_order_relaxed);
        // Under 1.5 GiB free: at 1 GiB the graphics allocations in OOT's Hyrule Field already failed
        // (the texture cache's out-of-memory recovery, 1.5-2 s freezes, run fritchii-29).
        // 2.5 GiB (was 1.5): entering an area the graphics take 2.5 GiB within seconds, and what the
        // pool lacks then the render thread packs itself (0.5-1.6 s, runs fritchii-44 and -49).
        const bool short_now = packing && free_reader != nullptr && free_reader() < ShortOfFree;
        // Short: every 0.25 s, and what the pass left Cold is packed here, up to 128 MiB a time, so the
        // render thread finds memory free instead of packing for it (2.3 s, run fritchii-44).
        const auto interval = std::chrono::milliseconds(short_now ? 250 : packing ? 5000 : 20000);
        scanner_wake.wait_for(lock, interval, [] { return scanner_stop || scanner_kicked.load(std::memory_order_relaxed); });
        scanner_kicked.store(false, std::memory_order_relaxed);
        if (scanner_stop) break;
        lock.unlock();
        if (scan_requested.load(std::memory_order_relaxed)) ScanPass(*range);
        // Still short: the slots nothing touched since the last sweep are packed, and the rest made
        // Cold for the next round (not the ones that refused or were unpacked a moment ago). Entering
        // OOT's Hyrule Field the graphics took 2.6 GiB in seconds with nothing Cold to pack, and the
        // render thread ran out (runs fritchii-45, -46).
        if (packing && free_reader != nullptr && free_reader() < ShortOfFree) PackCold(*range, std::size_t{256} << 20, SIZE_MAX, true);
        if (packing) CompactStore(*range);
#ifdef PS5_NATIVE
        FillReserve();
#endif
        if (std::chrono::steady_clock::now() >= next_report) {
            ServiceReport(*range, "now");
            next_report = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        }
        lock.lock();
    }
}

// Once: does a write to a slot that shows the zero block come back here, with its address, and
// resume? A refusal keeps the guest's memory committed when it is handed out, as before.
bool ServiceAvailable() noexcept {
    int state = service_state.load(std::memory_order_acquire);
    if (state != 0) return state == 1;
    const char* step = "ok";
    long long fault_ns = -1;
    void* base = nullptr;
    if (!SparseAvailable()) step = "sparse";
    else if (!InstallServiceSignal()) step = "install";
    else if ((base = NewSparseRange(2 * SparseSlot, false)) == nullptr) step = "reserve";
    if (base != nullptr) {
        SparseRange* range = nullptr;
        {
            const std::lock_guard lock{sparse_mutex};
            range = SparseRangeOf(reinterpret_cast<std::uintptr_t>(base));
        }
        if (range == nullptr || !AddStates(*range)) {
            step = "states";
        } else {
            probe_range.store(range, std::memory_order_release);
            auto* word = reinterpret_cast<volatile std::uint64_t*>(static_cast<std::uint8_t*>(base) + SparseSlot + 64);
            const auto before = counters.faults.load(std::memory_order_relaxed);
            probe_expected.store(reinterpret_cast<std::uintptr_t>(word), std::memory_order_release);
            const auto start = std::chrono::steady_clock::now();
            *word = 0x5a17ed5a17ed5a17ull; // faults once: the slot shows the zero block
            fault_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
            probe_expected.store(0, std::memory_order_release);
            if (*word != 0x5a17ed5a17ed5a17ull) step = "value";
            else if (counters.faults.load(std::memory_order_relaxed) != before + 1) step = "fault-count";
            else if (probe_mismatch.load(std::memory_order_relaxed)) step = "fault-address";
            else if (SlotState(*range, 1) != SlotOwn || !Empty(SlotState(*range, 0))) step = "state";
            else {
                // Back to the zero block, then written again: a slot comes and goes.
                LockSlot(*range, 1);
                ReturnSlot(*range, 1);
                UnlockSlot(*range, 1, SlotZero);
                if (*word != 0) step = "return";
                else {
                    *word = 7;
                    if (*word != 7 || SlotState(*range, 1) != SlotOwn) step = "again";
                }
            }
            if (std::strcmp(step, "ok") == 0) {
                // Fenced and unfenced: read while fenced, written after.
                const std::uintptr_t at = SlotAddress(*range, 1);
                Fence(at);
                if (*word != 7) step = "fence-read";
                Unfence(at);
                *word = 9;
                if (std::strcmp(step, "ok") == 0 && *word != 9) step = "unfence";
            }
            if (std::strcmp(step, "ok") == 0) {
                // Two slots of the same bytes: one block, then copied apart by a write, then the last
                // one takes the block back.
                auto* other = reinterpret_cast<volatile std::uint64_t*>(static_cast<std::uint8_t*>(base) + 64);
                *other = 9;
                const bool shared = ScanShare(*range, 1, 0);
#ifdef PS5_NATIVE
                if (!shared || SlotState(*range, 0) != SlotShared || SlotState(*range, 1) != SlotShared) step = "share";
                else if (*other != 9 || *word != 9) step = "share-read";
                else {
                    *other = 10; // a copy of its own
                    if (*other != 10 || *word != 9 || SlotState(*range, 0) != SlotOwn) step = "share-copy";
                    else {
                        *word = 11; // the last one: the block is its own again
                        if (*word != 11 || *other != 10 || SlotState(*range, 1) != SlotOwn) step = "share-last";
                    }
                }
#else
                if (shared) step = "share-host";
#endif
            }
            if (std::strcmp(step, "ok") == 0) {
                // Slot 1 with one page of data at page 3: Fine (one 16 KiB page kept), read in place,
                // then a write makes it whole again with every byte where it was.
                LockSlot(*range, 1);
                if (!Empty(SlotState(*range, 1))) ReturnSlot(*range, 1);
                UnlockSlot(*range, 1, SlotZero);
                auto* page3 = reinterpret_cast<volatile std::uint64_t*>(static_cast<std::uint8_t*>(base) + SparseSlot + 3 * FinePage + 8);
                *page3 = 0x0f1e2d3c4b5a6978ull;
                const auto held = guest_committed.load(std::memory_order_relaxed);
                if (!ScanFine(*range, 1) || SlotState(*range, 1) != SlotFine) step = "fine";
                else if (*page3 != 0x0f1e2d3c4b5a6978ull || *word != 0) step = "fine-read";
                else if (held - guest_committed.load(std::memory_order_relaxed) != SparseSlot - FinePage) step = "fine-held";
                else {
                    *word = 13; // page 0: the slot is whole again
                    if (*word != 13 || *page3 != 0x0f1e2d3c4b5a6978ull || SlotState(*range, 1) != SlotOwn ||
                        (range->fine[1] & 0xf00) == 0)
                        step = "fine-write";
                }
            }
            if (std::strcmp(step, "ok") == 0) {
                // Slot 1 Cold, then Packed (its block back to the pool), then read: the bytes come back
                // in a new block and the slot is its own again. A console where this fails keeps
                // every written slot in memory (pack_requested off).
                *word = 21;
                LockSlot(*range, 1);
                const bool cold = SlotState(*range, 1) == SlotOwn && MakeCold(*range, 1);
                const bool packed = cold && Pack(*range, 1);
                UnlockSlot(*range, 1, packed ? SlotPacked : cold ? SlotCold : SlotOwn);
                if (!packed) pack_refused_step = "pack";
                else if (*word != 21 || SlotState(*range, 1) != SlotOwn) pack_refused_step = "unpack";
                pack_works.store(std::strcmp(pack_refused_step, "ok") == 0, std::memory_order_relaxed);
                DrainRetired();
            }
            for (std::size_t slot = 0; slot < 2; ++slot)
                if (SlotState(*range, slot) != SlotZero) {  // Shown too: the range is freed only reserved
                    LockSlot(*range, slot);
                    ReturnSlot(*range, slot);
                    UnlockSlot(*range, slot, SlotZero);
                }
            probe_range.store(nullptr, std::memory_order_release);
        }
        FreeSparsePages(base, 2 * SparseSlot);
    }
    ResetCounters();
    const bool ok = std::strcmp(step, "ok") == 0;
    service_refused = step;
    char line[128];
    const int length = std::snprintf(line, sizeof(line), "EDEN_CRAM_CHECK ok=%d step=%s fault_ns=%lld pack=%s\n", ok ? 1 : 0,
                                     step, fault_ns, pack_refused_step);
    if (length > 0) Report(line);
    service_state.store(ok ? 1 : 2, std::memory_order_release);
    return ok;
}

// The guest range starts being a service (when the platform passed the check).
void ServiceStart(SparseRange& range) noexcept {
    if (!service_requested.load(std::memory_order_relaxed) || !ServiceAvailable() || !AddStates(range)) {
        char line[96];
        const int length = std::snprintf(line, sizeof(line), "EDEN_CRAM off: %s\n",
                                         !service_requested.load() ? "switched-off" : service_refused);
        if (length > 0) Report(line);
        return;
    }
    ResetCounters();
    range.promote = true;
#ifdef PS5_NATIVE
    FillReserve();
#endif
    service_range.store(&range, std::memory_order_release);
    Eden::Fastmem::guest_write_fault.store(&HandleGuestWriteFault, std::memory_order_release);
    // From here a fault finds the slot: what nothing has written keeps no mapping.
    {
        const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
        for (std::size_t slot = 0; slot < slots; ++slot) {
            LockSlot(range, slot);
            const auto state = SlotState(range, slot);
            if (state == SlotShown) VoidSlot(SlotAddress(range, slot));
            UnlockSlot(range, slot, state == SlotShown ? SlotZero : state);
        }
    }
    {
        const std::lock_guard lock{scanner_mutex};
        scanner_stop = false;
    }
    scanner = std::thread(ScannerLoop, &range);
    char line[128];
    const int length = std::snprintf(line, sizeof(line), "EDEN_CRAM on: scan=%d dedup=%d fine=%d pack=%d reserve=%zuMiB\n",
                                     scan_requested.load() ? 1 : 0, dedup_requested.load() ? 1 : 0,
                                     fine_requested.load() ? 1 : 0, pack_requested.load() && pack_works.load() ? 1 : 0,
                                     reserve_count.load() * (SparseSlot >> 20));
    if (length > 0) Report(line);
}

void ServiceStop(SparseRange& range) noexcept {
    if (service_range.load(std::memory_order_acquire) != &range) return;
    {
        const std::lock_guard lock{scanner_mutex};
        scanner_stop = true;
    }
    scanner_wake.notify_all();
    if (scanner.joinable()) scanner.join();
    ServiceReport(range, "end");
    MeasureFinePages(range);
    // Without the service nothing answers a fault: every slot reads as zeroes again, and a Cold or
    // Packed slot is its own again first.
    {
        const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
        for (std::size_t slot = 0; slot < slots; ++slot) {
            LockSlot(range, slot);
            auto state = SlotState(range, slot);
            if ((state == SlotCold && Warm(range, slot)) || (state == SlotPacked && Unpack(range, slot))) state = SlotOwn;
            const bool shown = state == SlotZero && ShowZero(SlotAddress(range, slot));
            UnlockSlot(range, slot, shown ? SlotShown : state);
        }
    }
    DrainRetired();
    service_range.store(nullptr, std::memory_order_release);
#ifdef PS5_NATIVE
    EmptyReserve();
#endif
}

// What a guest range keeps when it is given back: each block once, shared ones too.
void ReleaseServiceBlocks(SparseRange& range) noexcept {
#ifdef PS5_NATIVE
    const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        if (range.guest) DropSlotAliases(range, slot);
        const auto state = SlotState(range, slot);
        if (state == SlotFine) {
            const std::size_t kept = FineKept(range, slot);
            if (kept != 0 && sceKernelReleaseDirectMemory(range.physical[slot], kept * FinePage) != 0) std::abort();
            guest_committed.fetch_sub(kept * FinePage, std::memory_order_relaxed);
        } else if (state == SlotOwn || state == SlotCold) {
            if (sceKernelReleaseDirectMemory(range.physical[slot], SparseSlot) != 0) std::abort();
        } else if (state == SlotPacked) {
            DropPacked(range, slot);
        } else if (state == SlotShared && UnshareBlock(range.physical[slot]) == 0) {
            if (sceKernelReleaseDirectMemory(range.physical[slot], SparseSlot) != 0) std::abort();
            guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
        }
        if (state == SlotOwn || state == SlotCold) guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
        range.owned[slot].store(false, std::memory_order_relaxed);
    }
#else
    const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        if (SlotState(range, slot) == SlotOwn || SlotState(range, slot) == SlotCold)
            guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
        if (SlotState(range, slot) == SlotFine) guest_committed.fetch_sub(FineKept(range, slot) * FinePage, std::memory_order_relaxed);
        if (SlotState(range, slot) == SlotPacked) DropPacked(range, slot);
        range.owned[slot].store(false, std::memory_order_relaxed);
    }
#endif
}
} // namespace

// ---- What the guest's kernel tells the service ----

// Development switches (dev-settings cram=off, cram_scan=off, cram_dedup=off), before the guest's
// memory is taken.
void SetGuestMemoryService(bool on, bool scan, bool dedup) noexcept {
    service_requested.store(on, std::memory_order_relaxed);
    scan_requested.store(scan, std::memory_order_relaxed);
    dedup_requested.store(dedup, std::memory_order_relaxed);
}

// Settings > Performance > Compact memory (off unless chosen): written slots with many pages of
// zeroes keep only the others. It gives back more memory, and a game that writes such a slot again
// waits for it to be made whole (a fault and a 2 MiB copy, about 0.3 ms).
void SetGuestMemoryFine(bool on) noexcept {
    fine_requested.store(on, std::memory_order_relaxed);
}

// Out of memory now (a graphics allocation failed): the slots Cold since the last pass are packed
// here, on the caller's thread, until about `want` bytes went back. What went back (0: nothing Cold).
namespace {
// Own slots a sweep may make Cold without taking any that refused or were unpacked a moment ago.
std::size_t SweepableNow(SparseRange& range, std::size_t slots, std::uint32_t now) noexcept {
    std::size_t count = 0;
    for (std::size_t slot = 0; slot < slots; ++slot)
        count += SlotState(range, slot) == SlotOwn && (range.pack_after == nullptr || range.pack_after[slot] <= now);
    return count;
}
std::size_t PackCold(SparseRange& range_ref, std::size_t want, std::size_t max_packs, bool sweep, bool emergency) noexcept {
    SparseRange* const range = &range_ref;
    const std::size_t slots = (range->end - range->begin.load(std::memory_order_relaxed)) / SparseSlot;
    std::size_t given = 0;
    // A slot locked now is skipped: the lock may be this thread's own (a caller in the middle of a slot).
    const auto locked = [range](std::size_t slot) {
        return (range->state[slot].load(std::memory_order_relaxed) & SlotLock) != 0;
    };
    std::size_t tried = 0;
    for (std::size_t slot = 0; slot < slots && given < want && tried < max_packs; ++slot) {
        if (SlotState(*range, slot) != SlotCold || locked(slot)) continue;
        ++tried;
        const auto before = counters.packs.load(std::memory_order_relaxed);
        ScanPack(*range, slot);
        if (counters.packs.load(std::memory_order_relaxed) != before) given += SparseSlot;
    }
    if (sweep && given < want) {
        // Not enough was Cold: every Own slot is now (an emergency sweep). What the caller's next
        // attempt, a few ms later, finds still Cold is packed then.
        std::size_t swept = 0;
        const std::uint32_t now = NowSeconds();
        // An emergency takes the slots unpacked a moment ago only when the others are not enough
        // (a render thread unpacking texture sources it had just packed, run fritchii-49).
        emergency = emergency && SweepableNow(*range, slots, now) < want / SparseSlot;
        for (std::size_t slot = 0; slot < slots; ++slot) {
            if (SlotState(*range, slot) != SlotOwn || locked(slot)) continue;
            // Not a slot that refused to pack (or was unpacked) a moment ago: it would be compressed
            // again for nothing, ~5 ms each (1870 refusals in run fritchii-45). In an emergency every
            // slot: with the waits kept there, nothing was left to pack (runs fritchii-46 to -48 ended).
            if (!emergency && range->pack_after != nullptr && range->pack_after[slot] > now) continue;
            LockSlot(*range, slot);
            const bool cold = SlotState(*range, slot) == SlotOwn && MakeCold(*range, slot);
            UnlockSlot(*range, slot, cold ? SlotCold : SlotState(*range, slot));
            swept += cold;
        }
        counters.colds.fetch_add(swept, std::memory_order_relaxed);
        counters.sweeps.fetch_add(1, std::memory_order_relaxed);
    }
    return given;
}
} // namespace
// For a thread that needs memory now (the graphics allocator, the heap): a few slots here (~5 ms
// each), the rest by the scanner, woken for it.
std::size_t GuestMemoryPackNow(std::size_t want) noexcept {
    SparseRange* const range = service_range.load(std::memory_order_acquire);
    if (range == nullptr || range->packed == nullptr || !pack_requested.load(std::memory_order_relaxed) ||
        !pack_works.load(std::memory_order_relaxed) || in_pack)
        return 0;
    // 64 MiB or more is a caller the scanner has not kept up with: all of it here (a stall, not an end).
    const bool emergency = want >= (std::size_t{64} << 20);
    const std::size_t given = PackCold(*range, want, emergency ? want / SparseSlot : 8, true, emergency);
    scanner_kicked.store(true, std::memory_order_relaxed);
    scanner_wake.notify_one();
    return given;
}

// The console's zram: Own slots nothing touched for a scan pass are kept compressed and their
// memory goes back (Cold, Packed). On in a session unless dev-settings cram_pack=off.
void SetGuestMemoryPack(bool on) noexcept {
    pack_requested.store(on, std::memory_order_relaxed);
}
// For the host checks: packing's own checks that failed (an alias left, bytes changed while packing,
// bytes unpacked wrong); 0 when packing kept every byte.
std::uint64_t GuestMemoryPackErrors() noexcept {
    return counters.pack_aliased.load() + counters.pack_changed.load() + counters.unpack_wrong.load();
}
// The pool's usable free memory, for the scanner's pace (headless/performance.cpp, the console).
void SetGuestMemoryPoolFree(unsigned long long (*reader)()) noexcept {
    pool_free.store(reader, std::memory_order_relaxed);
}

// The console's GPU capture (headless/gpu_replay.cpp), which has no dirty-page bits and reads every
// GPU-mapped page: true when the guest's memory at `at` reads as zeroes with nothing of its own
// (a Zero or Shown slot), so the capture need not read it (a Zero slot would fault in the zero block).
bool GuestMemoryReadsZero(const void* at) noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(at);
    SparseRange* const range = WriteRangeOf(address);
    if (range == nullptr) return false;
    return Empty(SlotState(*range, (address - range->begin.load(std::memory_order_acquire)) / SparseSlot));
}

// The guest's kernel hands pages out: they are committed now only without the service.
void CommitGuestMemoryOnHandOut(std::uint64_t offset, std::size_t bytes) noexcept {
    SparseRange* const range = guest_range.load(std::memory_order_acquire);
    if (range != nullptr && range->promote) return;
    CommitGuestMemory(offset, bytes);
}

// The guest's kernel fills pages (`at` is the backing at `offset`). With the service, a fill with
// zeroes leaves Zero slots alone and gives whole Own or Shared slots back; anything else is written.
void ClearGuestMemory(std::uint8_t* at, std::uint64_t offset, std::size_t bytes, int fill) noexcept {
    SparseRange* const range = guest_range.load(std::memory_order_acquire);
    if (range == nullptr || !range->promote || fill != 0) {
        if (range != nullptr && !range->promote) CommitGuestMemory(offset, bytes);
        std::memset(at, fill, bytes); // (with the service, a write to a Zero slot promotes it)
        return;
    }
    const std::uint64_t end = offset + bytes;
    for (std::uint64_t cursor = offset; cursor < end;) {
        const std::size_t slot = static_cast<std::size_t>(cursor / SparseSlot);
        const std::uint64_t slot_end = (cursor / SparseSlot + 1) * SparseSlot;
        const std::uint64_t piece_end = std::min(end, slot_end);
        const bool whole = cursor % SparseSlot == 0 && piece_end == slot_end;
        if (whole) {
            LockSlot(*range, slot);
            const auto state = SlotState(*range, slot);
            if (state != SlotZero) {
                if (!Empty(state)) counters.returns_clear.fetch_add(1, std::memory_order_relaxed);
                ReturnSlot(*range, slot);
            }
            UnlockSlot(*range, slot, SlotZero);
        } else if (!Empty(SlotState(*range, slot))) {
            std::memset(at + (cursor - offset), 0, static_cast<std::size_t>(piece_end - cursor));
        }
        cursor = piece_end;
    }
}

// A page the guest's kernel fills one at a time (optimized allocation): the same rule.
void FillGuestMemory(std::uint8_t* at, int fill, std::size_t bytes) noexcept {
    SparseRange* const range = guest_range.load(std::memory_order_acquire);
    if (range != nullptr && range->promote && fill == 0) {
        const auto begin = range->begin.load(std::memory_order_relaxed);
        const auto address = reinterpret_cast<std::uintptr_t>(at);
        if (address >= begin && address < range->end && Empty(SlotState(*range, (address - begin) / SparseSlot)))
            return;
    }
    std::memset(at, fill, bytes);
}

// The guest's kernel freed pages: every slot they cover whole goes back.
void GuestMemoryFreed(std::uint64_t offset, std::size_t bytes) noexcept {
    SparseRange* const range = guest_range.load(std::memory_order_acquire);
    if (range == nullptr || !range->promote || bytes < SparseSlot) return;
    const std::uint64_t first = (offset + SparseSlot - 1) / SparseSlot;
    const std::uint64_t last = (offset + bytes) / SparseSlot; // exclusive
    for (std::uint64_t slot = first; slot < last; ++slot) {
        LockSlot(*range, static_cast<std::size_t>(slot));
        if (const auto state = SlotState(*range, static_cast<std::size_t>(slot)); state != SlotZero) {
            if (!Empty(state)) counters.returns_free.fetch_add(1, std::memory_order_relaxed);
            ReturnSlot(*range, static_cast<std::size_t>(slot));
        }
        UnlockSlot(*range, static_cast<std::size_t>(slot), SlotZero);
    }
}

// For the host checks: one scan pass now, and a slot's state (0 Zero, 1 Own, 2 Shared, 3 Fine, 4 Shown, 5 Packed,
// 6 Cold, -1 none).
void GuestMemoryScanNow() noexcept {
    if (SparseRange* const range = service_range.load(std::memory_order_acquire)) ScanPass(*range);
}
int GuestMemorySlotState(std::uint64_t offset) noexcept {
    SparseRange* const range = guest_range.load(std::memory_order_acquire);
    if (range == nullptr || range->state == nullptr) return -1;
    return SlotState(*range, static_cast<std::size_t>(offset / SparseSlot));
}

// ---- The 64-bit fastmem window (src/host_memory.cpp, Window64) ----


// From a JIT fault in the window: shows the Own slot at `backing` (the guest's memory) at `window_at`,
// read-write. False when the slot is not Own (or the platform cannot alias it): the access then
// takes the page-table path. Both addresses are 2 MiB aligned.
bool AliasGuestSlot(std::uintptr_t backing, std::uintptr_t window_at, bool writable) noexcept {
#ifdef PS5_NATIVE
    auto* range = guest_range.load(std::memory_order_acquire);
    if (!range || !range->state || !range->aliases) return false;
    const std::uintptr_t begin = range->begin.load(std::memory_order_relaxed);
    if (backing < begin || backing >= range->end || (backing - begin) % SparseSlot || window_at % SparseSlot) return false;
    const std::size_t slot = (backing - begin) / SparseSlot;
    LockSlot(*range, slot);
    const std::uint8_t state = SlotState(*range, slot);
    bool shown = false;
    if (state == SlotOwn && range->physical[slot] >= 0) {
        const int protection = writable ? PROT_READ | PROT_WRITE : PROT_READ;
        const std::uintptr_t entry = window_at | (writable ? 0 : AliasReadOnly);
        std::uintptr_t* free_entry = nullptr;
        for (unsigned i = 0; i < AliasesPerSlot; ++i) {
            auto& alias = range->aliases[slot * AliasesPerSlot + i];
            if ((alias & ~AliasFlags) == window_at) {
                // Another core showed it first, or it is read-only and this access may write now,
                // or it shows only pieces and is now shown whole.
                shown = alias == entry || (!writable && !(alias & AliasSmall)) ||
                        (MapSlot(window_at, protection, range->physical[slot]) && ((alias = entry), true));
                free_entry = nullptr;
                break;
            }
            if (!alias && !free_entry) free_entry = &alias;
        }
        if (!shown && free_entry && MapSlot(window_at, protection, range->physical[slot])) {
            *free_entry = entry;
            shown = true;
            aliases_made.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!shown) aliases_refused.fetch_add(1, std::memory_order_relaxed);
    UnlockSlot(*range, slot, state);
    return shown;
#else
    (void)backing;
    (void)window_at;
    return false; // the host's guest memory is private: nothing to alias
#endif
}

// From a JIT fault in a window region that cannot show a whole block (partly mapped, partly
// protected, or not on a 2 MiB boundary of the backing): shows the 16 KiB at `backing` of an Own
// slot at `at`, inside the window region `region_at`. The slot's entry names the whole region, so
// any change of the slot voids it. Each region shown this way costs one CPU page table.
// `slot_read_only`: the slot's block is shown read-only whatever `writable` asks.
bool AliasGuestPage(std::uintptr_t backing, std::uintptr_t at, std::uintptr_t region_at, bool writable, bool write,
                    bool* promote, bool* slot_read_only) noexcept {
#ifdef PS5_NATIVE
    auto* range = guest_range.load(std::memory_order_acquire);
    if (!range || !range->state || !range->aliases) return false;
    const std::uintptr_t begin = range->begin.load(std::memory_order_relaxed);
    if (backing < begin || backing >= range->end || (backing - begin) % FinePage || at % FinePage) return false;
    const std::size_t slot = (backing - begin) / SparseSlot;
    LockSlot(*range, slot);
    const std::uint8_t state = SlotState(*range, slot);
    bool shown = false;
    // Another state shows a block read-only: Zero and Shown the zero block, Shared the shared one,
    // Fine its page (the zero block or its compact copy). A write there promotes the slot first.
    const std::size_t offset = backing - SlotAddress(*range, slot);
    std::int64_t physical = -1;
    if (state == SlotOwn || state == SlotShared) {
        physical = range->physical[slot] >= 0 ? range->physical[slot] + std::int64_t(offset) : -1;
    } else if (Empty(state)) {
        physical = zero_block;
    } else if (state == SlotFine && range->physical[slot] >= 0) {
        const std::uint64_t* bits = FineZero(*range, slot);
        const std::size_t page = offset / FinePage;
        const auto zero = [bits](std::size_t i) { return (bits[i / 64] >> (i % 64)) & 1; };
        if (zero(page)) {
            physical = zero_block;
        } else {
            std::size_t next = 0;
            for (std::size_t i = 0; i < page; ++i) next += !zero(i);
            physical = range->physical[slot] + std::int64_t(next * FinePage);
        }
    }
    *slot_read_only = state != SlotOwn;
    if (state != SlotOwn) {
        writable = false;
        if (write) physical = -1, *promote = true;
    }
    // Cold or Packed: any access takes the slot back first (the caller's guest_write_fault), then shows it.
    if (state == SlotCold || state == SlotPacked) *promote = true;
    if (physical >= 0) {
        std::uintptr_t* entry = nullptr;
        bool known = false;
        for (unsigned i = 0; i < AliasesPerSlot && !known; ++i) {
            auto& alias = range->aliases[slot * AliasesPerSlot + i];
            if ((alias & ~AliasFlags) == region_at) entry = &alias, known = true;
            else if (!alias && !entry) entry = &alias;
        }
        void* address = reinterpret_cast<void*>(at);
        if (entry && sceKernelMapDirectMemory(&address, FinePage, writable ? PROT_READ | PROT_WRITE : PROT_READ, MAP_FIXED,
                                              physical, FinePage) == 0 && address == reinterpret_cast<void*>(at)) {
            if (!known) aliases_made.fetch_add(1, std::memory_order_relaxed);
            *entry = region_at | AliasSmall;
            shown = true;
        }
    }
    if (!shown) aliases_refused.fetch_add(1, std::memory_order_relaxed);
    UnlockSlot(*range, slot, state);
    return shown;
#else
    (void)backing;
    (void)at;
    (void)region_at;
    (void)writable;
    (void)write;
    (void)promote;
    (void)slot_read_only;
    return false;
#endif
}

// The window region at `window_at` no longer shows the slot at `backing` (guest unmap, protection).
// True when it did.
bool DropGuestAlias(std::uintptr_t backing, std::uintptr_t window_at) noexcept {
    auto* range = guest_range.load(std::memory_order_acquire);
    if (!range || !range->aliases) return false;
    const std::uintptr_t begin = range->begin.load(std::memory_order_relaxed);
    if (backing < begin || backing >= range->end) return false;
    const std::size_t slot = (backing - begin) / SparseSlot;
    bool dropped = false;
    LockSlot(*range, slot);
    for (unsigned i = 0; i < AliasesPerSlot; ++i)
        if (auto& alias = range->aliases[slot * AliasesPerSlot + i]; (alias & ~AliasFlags) == window_at) {
            VoidWindowRegion(window_at);
            alias = 0;
            dropped = true;
            aliases_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    UnlockSlot(*range, slot, SlotState(*range, slot));
    return dropped;
}

void GuestAliasCounts(std::uint64_t* made, std::uint64_t* dropped, std::uint64_t* refused) noexcept {
    *made = aliases_made.load(std::memory_order_relaxed);
    *dropped = aliases_dropped.load(std::memory_order_relaxed);
    *refused = aliases_refused.load(std::memory_order_relaxed);
}
