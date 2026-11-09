# ProsperoEden-heap (eglantine-heap), 2026-10-31

Commits (local, not pushed): 21c33ac census, b8902a3 trim (plugs), 102a19f merge of
eglantine-base 6329cab, a63dfee trim fix. Host check tools/check-heap-growth.py passes
(it now covers trim and unplug under 8 threads + the thread sanitizer).

## What the heap holds (EDEN_PERF_HEAP_USE, new, every 5 s in dev builds)
Race, Mario Kart World, standard dev settings, heap_trim=off:
- pieces 2176-2304 MiB; in use in the spaces 1460-1600 MiB (arenas' blocks included).
- arenas: 32 x 8 MiB, 175 MiB used (~80 MiB free).
- blocks 8-32 MiB: ~510-540 MiB live, stable; 256 KiB-2 MiB: 125 MiB, up to ~320 MiB during a
  course load. Large blocks (>= 32 MiB, own memory): 270-320 MiB in 4-5 blocks.
- Live rises ~360 MiB during a course load, which is when the pool runs out.
- The last pieces stayed mostly empty (58/13/7 MiB of 128), held by a few small blocks: small
  requests followed the newest piece (the space hint).

## The change: plugs (headless/heap_arenas.inc, src/memory_pages.cpp)
Every 5 s the GPU thread calls eden_heap_trim. It mallocs free stretches of 4-64 MiB out of each
space and releases the 2 MiB pages inside each block (nothing writes inside a block in use).
Before the heap would grow, it recommits a plug and frees it. The trim also resets the space hint
so the last pieces drain, and skips a round after an unplug (else trim and the allocator take
turns). EDEN_PERF_HEAP pieces= is now memory held (pieces less given back).
dev-settings `heap_trim=off` = the old behavior.

Result, same base 6329cab: race 2304 MiB held -> 2074-2170 MiB (135-230 MiB less), menus
1920 -> 1850 MiB. Race fps 18-35 vs 18-34. Trim costs 80-520 us on the GPU thread per 5 s.
That is far from the ~1 GB the OOMs need. Of the 4 trim runs, 2 ended in the same pool OOM at the
race (guest 5.7 GB + graphics 4.6 GB; largest free 8-14 MiB). Of the 3 off runs, 1 hit the OOM and
1 hit an AssertFatal in BufferCache::UpdateGraphicsBuffers (1.4 GB free).

Bug found and fixed on the way: unmapping and then reserving with MAP_FIXED leaves a window where
another mapping at the same hint (guest page table slots) lands and is then replaced
(heap-trim3: SIGSEGV in PromoteZero). The reserve now goes straight over the mapping.
CommitMemoryRange's undo had the same window.

## What is left (~450-550 MiB held over in use in races)
- Holes under 4 MiB inside pieces (~400 MiB): not reachable at 2 MiB pages. 64 KiB decommit would
  cost TLB reach and give the GPU holes it cannot use.
- Arenas ~80 MiB free (4 MiB arenas would halve it; allocation speed then needs a check).
- Lowering the peak: the course load adds ~360 MiB live. Lead from the coordinator: dynarmic
  per-block tables (patch_information, fastmem_patch_info; PokeDinner a2cc455) may be a large
  part of the 1.5-1.7 GB live. Needs the PC heapsite profile to confirm.

## Paused (2026-10-31, user request)
Where it stands: all work above is committed on eglantine-heap (a63dfee is the tip). The working
tree has one uncommitted change, headless/CMakeLists.txt (a VFS open-file cache fix, ~62 MiB).
It is the census session's work in progress in this same worktree, so I left it alone. No build
or console run of mine is in progress. The heap-peak census session
(local_dedaa073) was started from here. I asked it to pause and to write its state in
notes/ProsperoEden-heap-peak.md.

## Live heap census by call site (2026-10-31, PC heapsite)
PC ns2-host of a63dfee, EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1, title -> menus -> a course by 240 s.
Profiler: heapsite.c with 8 return addresses, the aligned allocators, a size histogram, and a dump
of every site >= 256 KiB every 10 s to /work/hs/ (heapsite/heapsite2.c; heapsite/rank.py <eden-headless> <dump> symbolizes and ranks by owner; heapsite/heapcmp.py heap.txt... summarizes console runs).
PC live 1.35 GB at 100 s, 1.50 at 150 s, 1.58 at 180 s (driving), 1.67 at 320 s (race). The
NVIDIA driver's own share is ~100 MiB of that; the console's RADV/ACO share is not visible here.

Stable from the menus on (MiB, at the race):
| MiB | site | blocks | console |
| ---: | --- | --- | --- |
| 192 | RomFS of the loose romfs: built twice (Loader::...deconstructed_rom_directory and LazyRomFs::Read, ~65 each: VisitDirectory, ConcatenatedVfsFile, Build) + the open-file cache (62, below) | 1.8 M | same code |
| 128+64 | Fermi2D software blit: src/dst ScratchBuffers 2x64 + its GuestMemory 54-64, kept after one big blit (~100-150 s) | 3 | not seen in the console's large blocks; PC-only fallback? |
| 120 | UpdateDescriptorQueue payloads: guest 0x80000 x 8 frames x 24 B = 96, compute 24 | 2 | same (one of the console's >= 32 MiB blocks) |
| 96 | A64 fast_dispatch_table, 0x100000 x 16 B inline in each Jit::Impl, 6 cores | 6 | same (8-32 MiB class) |
| 88 | KDynamicPageManager (KSecureSystemResource 64 + resource managers 24) | 2 | same |
| 88 | Fiber stacks, 512 KiB per guest thread (175) | 175 | stack size may differ |
| 83 | JitGroup::Register: blocks/sites flat maps (31 + 23.5, rehash doubling) + per-target Site vectors (12, 263k) | 264 k | same; grows with code (49 -> 83) |
| 96 | DescriptorTable TIC/TSC: 3 x 16 MiB each (limit 0x7ffff x 32 B) | 6 | same (8-32 MiB class) |
| 67.5 | Tegra::MemoryManager page tables (5) | 5 | same |
| 40 | KMemoryManager::Impl (4 x 10) | 4 | same |
| 34 | IPlatformServiceManager (shared fonts, 2 x 17) | 2 | same |
| 24+19+16+8 | Tegra::GPU, Core::System, DeviceMemoryManager, GpuSplit | 4 | same |
| 50 | Shader IR ObjectPools (Inst 37.5, Block 12) kept at their high water | 2 | same |
| 14.5 | BlockRangeInformation pages | 6 k | same |
The 8-32 MiB blocks (~520 MiB on the console) are mostly fast_dispatch_table 96 + TIC/TSC 96 +
MemoryManager 67 + compute descriptor queue 24 + KMemoryManager 40 + fonts 34 + GPU/System/DMM ~60
+ flat-map rehashes.
Growing 150 -> 320 s (PC): driver shader modules/pipelines (+60), JitGroup (+15), VMA blocks
(+12), image views (+8). The PC's course load adds ~170 MiB, not the console's +360: the rest is
the console's own (RADV/ACO pipelines, port caches), which this profiler cannot see.
Dynarmic lead (PokeDinner a2cc455): patch_information and fastmem_patch_info do not show at all
(none >= 256 KiB): the shared JIT (headless/dynarmic) links through JitGroup and the pinned patch
lookup creates no entries. block ranges are 14.5 MiB. Nothing left to take from that commit.

## Cut: the open-file cache's dead entries (headless/CMakeLists.txt, vfs_real.cpp)
RealVfsFilesystem::cache maps a path to a weak_ptr and never dropped an entry once the file was
gone: node + path + the RealVfsFile's whole make_shared block stayed, for every one of the
205,781 romfs files the RomFS build lists. ~RealVfsFile now erases its entry when it is expired.
Console, same base a63dfee, standard dev settings:
| run | game_loaded live | in_use menus | in_use race (median) | held race (median) | race fps |
| --- | --- | --- | --- | --- | --- |
| heap-vfs-base (OOM in the race, 4 windows) | 706 MiB / 2.41 M blocks | 1422-1679 | 1511-1727 (1638) | 1852-2048 (1885) | 22.7 |
| heap-trim4 (same code, earlier) | 706 / 2.40 M | 1424-1645 | 1467-1857 (1628) | 1788-2170 (2088) | 24.8 |
| heap-vfs-fix (ran to the end) | 579 MiB / 1.17 M blocks | 1293-1623 | 1526-1756 (1566) | 1906-2048 (1989) | 23.9 |
Live -127 MiB and -1.24 M blocks at game load (twice the PC's 62 MiB / 0.6 M; why not checked), -60 to
-70 MiB median in races. Held in races is not lower: the trim/fragmentation noise between runs
(+-100 MiB) is larger than what the freed small blocks give back.

## Next cuts by size (not done)
1. RomFS built twice for the same loose romfs (~65 MiB): Loader and LazyRomFs each build one.
2. A64 fast_dispatch_table 0x100000 -> 0x40000 per core: -72 MiB; needs a guest-CPU perf check.
3. Guest descriptor queue 0x80000/frame: measure the per-frame high water in races first; 0x20000
   would give back 72 MiB if it never overflows (an overflow wraps = glitches).
4. TIC/TSC tables sized to the guest's limit (0x7ffff): 96 MiB, mostly never read; a sparse table
   touches the GPU thread's hot path.
