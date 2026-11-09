# 64-bit fastmem on the console: design (2026-11-03)

## Why
PC race A/B (Mario Kart World, same JIT, Vulkan): upstream-style 64-bit fastmem arena 22.7 ms
mean frames (44.0 fps, median 21.3) against 26.5 ms (37.7 fps, median 24.0) without: about +17%.
One run each; the fastmem arm runs with CRAM off (the PC's CRAM-off path without fastmem is broken,
0 presents). Runs: coordination/runs/pcbase-1, pcfm-on-fix. Fault fix that made it work: 30550d0
(a fastmem fault invalidates its block group-wide; before it, 61 million faults per race).

## Console constraints (measured)
- Guest RAM backing (CRAM) at 0x80_0000_0000, 20 GiB; RADV's device memory owns
  [0x40_0000_0000, 0x80_0000_0000); user space ends at 1 TiB. A 256 GiB window fits above the
  backing (e.g. 0xA0_0000_0000), 2 MiB aligned.
- MKW's guest layout (EDEN_GUEST_LAYOUT): code 0x8000000, stack 0x10_85c0_0000+2 GiB, alias
  0x11_05c0_0000+64 GiB, heap 0x21_05c0_0000+16 GiB: all below 2^38, so 38 window bits cover it.
  The JIT's fastmem range check must use 38 bits (conf.fastmem_address_space_bits), not 39.
- CPU page tables: 16,384 per title (one per 2 MiB mapped at 4/16 KiB granularity); in races only
  ~2,530 are free (EDEN_CRAM page_tables_free). 16 KiB aliases for ~2,500 written slots would use
  them all. 2 MiB large-page mappings use none.
- Layout scan (EDEN_HOST_LAYOUT_SCAN, PC, menus): 8,682 MiB mapped in 4,470 regions of 2 MiB;
  4,218 regions (97.2% of mapped memory) map linearly onto one 2 MiB-aligned stretch of backing.

## Design: lazy 2 MiB large-page aliases
1. Window: reserve 256 GiB (sceKernelReserveVirtualRange, 2 MiB aligned) above the backing. It is
   the page table's fastmem arena for 64-bit games; every address starts unmapped.
2. Region state (per 2 MiB of guest address, only regions Eden mapped): backing offset if all 512
   pages are mapped linearly onto one 2 MiB-aligned backing stretch, and whether all pages are
   readable and writable (no GPU tracking, guest permissions). Kept from HostMemory::Map/Unmap/
   Protect, which Eden calls for every change while the arena is set.
3. Fault in the window (handler, before dynarmic's fallback): region eligible -> map the slot's
   current block as one 2 MiB mapping and retry the access:
   - Own slot: its direct memory, read-write.
   - Shown/Zero slot: the shared zero block, read-only; Shared: its shared block, read-only. A write
     then faults again: let CRAM promote the slot (as HandleGuestWriteFault does), remap read-write.
   - Fine slot (16 KiB pages), non-linear or partly protected region: not aliasable -> dynarmic's
     fallback, and the site is recompiled onto the page-table path (group-wide, 30550d0).
4. Invalidation: before CRAM changes a slot (promote, return to Zero, share, make Fine, free) its
   window aliases are unmapped (reverse map slot -> regions; one physical slot can sit under several
   guest addresses, e.g. the alias region). Unmap before the backing changes, never after: a stale
   alias would reach freed memory. Map/Unmap/Protect on a region unmap its alias too.
5. Faults only demote sites for accesses that cannot be made direct; read traps on GPU-tracked
   pages keep their path (the page-table code calls HandleRasterizerDownload as today).

## Risks
- Console power-off was once seen with many aliases of one block (old port): watched runs, start
  with a cap on aliased regions.
- MKW writes terrain tiles that are already textures (switch2mac): GPU write tracking must unmap
  the region's alias (step 4) or the ground goes flat.
- Signal-handler work: mapping system calls and the region lookup must be lock-free or use locks
  the faulting thread can never hold.

## Stages
1. Window reserved, arena set, 38-bit range check, every access faults to the fallback and is
   demoted: correctness of the plumbing, fault count, no aliasing.
2. Own slots aliased read-write on first fault.
3. Zero/Shown/Shared read-only aliases with promotion on write; CRAM transitions and Protect
   invalidation.
4. Pro A/B races.

## Page-table quota probe (2026-11-03, read-only, exported APIs only)
coordination/pmc/parse-probe.py over runs/probe-4; dev-settings probe_budget=on (headless/performance.cpp,
dev builds only). Every call got a scratch block, pattern-filled; no setter was called.
- sceKernelGetPageTableStats works: CPU pool 16384 total, 16192 free at idle (~192 used); GPU pool
  16384/16384. In races CPU free falls to ~2,530 (EDEN_CRAM page_tables_free): game + emulator use ~13,850.
- sceKernelMapperGetParam / MapperGetUsageStatsData answer (struct-size in the first word, Sony's usual
  self-sizing query). They describe the GPU mapper (512 GiB region 0x80000000000..0x88000000000), not the
  CPU page-table pool. GetBudget/SetBudget are NOT in the SDK's libkernel stub (not linkable here).
- No exported setter for the CPU page-table pool size (16384) exists: the only Set is
  MapperSetPageTablePoolOccupancyNotificationThreshold (a notification threshold, not the size).

Conclusion: raising the 16,384 CPU page-table pool so the 64-bit window can map at 16 KiB cannot be done
through exported APIs. It would need a kernel write (allowed since the rule change, [[firmware-reference-rules]]),
finding the per-process pool-size field in the kernel and raising it for our process only — real crash/power-off
risk for an uncertain console gain (the PC's +17% came from full coverage the console's 2 MiB aliases don't reach).

## Learn/use races with the refusal census (2026-11-03 afternoon, 29de6b4)
- fmq-base1 (no window) 43.8 fps. fmq-learn raced but the stop hung (restart overwrote the windows).
- fmq-use1 loaded 187,902 demoted blocks, yet demotions kept growing (to 348k) and it crashed at 218 s
  with GPU_FAULT_PAGE_FAULT_ASYNC, like fmp-use1 (221 s). Two of two "use" runs: treat as fastmem-related.
- Census: only ~350 of ~4,470 mapped regions refuse, but they draw most faults. partial ~177 (areas
  0x10 = stacks, 0x11 = alias region; 37 of them at least 7/8 mapped), blocked ~106 and blocked_write
  ~45 (heap, 0x21-0x22), slot ~19, unaligned ~9 (code area), inconsistent 2.
- Next: show those regions at 16 KiB (one CPU page table each: ~350 against ~2,500 free in races) so
  their sites stop faulting and demoting; find why demotions still grow after a load.

## 2026-11-03 evening: pieces, slot states, the stop-hang deadlock
- 16 KiB pieces (6e6bc78) for regions that cannot be shown whole; protection changes mprotect only
  the pieces (whole-region voids had 160k drops a race). Slot states (290dd1a): Zero/Shown/Shared/
  Fine shown read-only, a write promotes the slot first. Pro (fps, median): baseline 42.9-44.7;
  s1-learn (pieces, voids) 36.7; s2-use1 37.5; s3-learn (slot states) 41.5/42.3; s3-use1 42.1/43.3.
  Window on costs the GPU thread ~10% per draw (9.7 vs 8.8 us) and guest writes into GPU-tracked
  memory 1.9% -> 6% of the window (write-protected pieces trap first).
- Use runs crash at the first course load more often (3 of 5): GPU_FAULT_PAGE_FAULT_ASYNC right after
  radv/ps5 places a 562 MiB allocation "in 16 pieces" (3 in runs that live). Fragmentation of direct
  memory with the window on; a driver-side look is due.
- Remaining refusals: ~250k a race in partly mapped pieces (stack area 0x10, alias area 0x11).
  Samples of those regions' mapped pages: s4-*.
- Demotions are per site from the next build (key: block ^ IR index * golden ratio), list file
  <title>.fastmem64-sites.
- Stop hang = lock-order deadlock (0041): GPU thread holds the rasterizer lock and asks the
  GPU-memory registry lock (BackSparse) / nx2:gpu region lock; a service thread held those while a
  registry listener asked for the rasterizer lock. PC thread dump (pc-stophang.sh, EDEN_HOST_STOP_DUMP).
  Since 0041: Pro lk-base1..3 42.3/44.7/43.6 fps, 3 clean stops; PC pcfix-* clean.
- console-run.sh fetches heap/eden_log before the stop and keeps them if a restart overwrote them.
