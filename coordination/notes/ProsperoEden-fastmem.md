# ProsperoEden-fastmem (lever-fastmem): 64-bit fastmem window on the Pro

## 2026-11-03
- Saved demotion lists never matched a later session: the game's code loads at a different address
  each session (Module main at 0x80334000, 0x80508000, 0x80770000, 0x809e0000, 0x81014000 in
  s3..s7), and the keys held absolute PCs (decoded the console's list: 178,504 keys, 57k blocks,
  all in 0x80-0x84xxxxxx). "use" races therefore learned everything again (+47k demotions by the
  first window, as in learn) and on top took ~178k unrelated sites off fastmem: why use races were
  the slow ones (37-42 vs 42.5-43.8 learn).
- Fix: key = block location ^ (IR index << 40); the list is saved relative to the game's code run
  (entry point onward, the JIT block list's range) as `<title>.fastmem64-code-sites`; old lists
  are ignored. Sites outside the game's code are not kept.
- Census adds track_calls/track_mtsc (TSC ticks in the window's Map/Unmap/Protect: Eden's mapping
  and the GPU thread's cache marking), fault_mtsc and run_mtsc.
- fms.sh takes CHECKOUT=<worktree>.
- Build note: this environment needs PROSPERO_TOOLCHAIN_BIN=$HOME/.local/bin:<dir with gmake -> make>.

### Series f1 (11b6c46, interleaved, same build; settings gpu_accuracy=low cram_fine=on cram_dedup=on)
| run | fps (median) | us/draw | faults | demoted new | end |
| --- | --- | --- | --- | --- | --- |
| learn | 40.10 (40.50) | 9.89 | 289,744 | 213,300 | clean |
| base1 | 42.36 (43.35) | 9.06 | - | - | clean |
| use1 | 45.85 (47.35) | 8.94 | 23,462 | +19k | clean |
| base2 | 42.36 (44.29) | 9.43 | - | - | clean |
| use2 | 44.00 (44.86) | 9.21 | 26,488 | +22k | clean, worst frame 131 ms vs 446 |
| base3 | 42.87 (44.24) | 8.99 | - | - | clean |
| use3 | 45.35 (46.03) | 9.00 | 5,762 | +4.6k | clean |
- Window with a matching list: mean 45.07 vs 42.53 fps, +6.0%; 3 of 3 pairs ahead. 0 crashes or GPU
  faults in 7 races. Textures: use3/base3 frames at 380 s and 410 s look alike (no black or missing).
- The GPU thread's per-draw penalty (~10%) was the learning storm, not the window: gone in use runs.
- The learning run is the expensive one (each demotion recompiles its block and halts every core):
  a title's first session with the window runs ~5% under the baseline.
- Window Map/Unmap/Protect: ~1.8M calls a race, 3.3% of a thread in learn (~7 us a call), 1.2% in use.
- Refusals stay in partly mapped regions (stack area 0x10, alias area 0x11): 254k a learn race with
  unmapped_access=0 and inconsistent=0; 87247c0's per-page backing did not remove them. The next build
  says why (piece_read_blocked / piece_write_blocked / piece_scattered, with samples).
- CPU page tables: ptf_min ~1,340-1,600 with the window vs ~2,550-2,700 without.

### Series f2 (7af096d: window on by default; "def" = no fastmem64 setting, base = fastmem64=off)
| pair | def fps (median) | base fps (median) |
| --- | --- | --- |
| 1 | 47.60 (49.07) | 42.33 (43.20) |
| 2 | 44.94 (46.91) | 42.41 (42.76) |
| 3 | 44.90 (44.56) | 43.90 (44.48) |
- Mean 45.81 vs 42.88, +6.8%; 6 clean stops, 0 GPU faults. Faults a race 1.2-2.1k; the list grows
  by ~1-2k sites a session (263,555 after def3).
- Census (def1): Protect 1.0% of a thread (47k piece mprotects, vs 241k in f1-learn before the
  unchanged-protection skip), faults 0.6%. Remaining refusals: piece_read_blocked 675 (read traps),
  scattered 5. Scattered sample: guest 0x116eeabe08, pages backed by 0x37e72f,0x37e730 | 0x37e7e1,
  0x37e7e2 (two 8 KiB runs: thread TLS/stack pages from different heap pages).
- f1+f2: window ahead in 6 of 6 pairs, 45.44 vs 42.71 fps (+6.4%); 13 races, 0 crashes.

### Open
- The first session of a title (no list) learns ~210-230k sites: 40.1 fps in f1-learn, 45.0 in
  f3-def1. Every demotion recompiles its block and halts all six cores (jit_impl.inc
  PerformRequestedCacheInvalidation); a lazier invalidation (only the faulting core) would cut that.
- A32 titles get the window too (default on); not raced on any 32-bit title.

### Series f3 (9d59a5a: list header "EFS1" + build ID)
- def1: the older headerless list was ignored (demoted_loaded=0), relearned: 45.02 fps (median 47.14).
- def2: loaded 225,830 sites: 46.36 (48.89). base1 (fastmem64=off): 43.32 (44.41). 3 clean stops.
- Totals f1-f3: window with a list ahead in 7 of 7 pairs; 16 races, 0 crashes, 0 GPU faults.
- FPS overlay (GPU timing) turned on at ~20:20 by the coordinator. Launch times: f2 pair 1 at 19:52/19:58
  and pair 2 at 20:04/20:16, so both started with it off (base2 was still running at 20:20). Pair 3
  (20:28/20:34) and all of f3 (20:44-21:02) started with it on. No pair is split across the change,
  assuming the setting is read at launch.

## 2026-11-03/04 night: round 2 (merged overnight-60fps 8b36c60)
- Window share in hot code (g1-share: pc_sample=on pc_core=0 pc_fast=on, list loaded; tool
  coordination/fmshare.py, which reads the hot blocks' host code dumps): guest memory access sites in
  the dumped hot blocks, weighted by samples: **window 15%, page table 85%**. Page-table walk
  instructions take 10.7% of guest core 0's samples (10.9% with no window, g0map-3). The guest code of
  the same blocks: memory instructions 66% pointer-based, 21% SP-based (never windowed by design),
  12.5% x29-based. So most hot pointer sites are demoted: one bad access takes a site off for good.
- PC comparison: the upstream-style arena (EDEN_HOST_A64_FASTMEM) takes ~16k faults a race
  (pcfm-on-fix), against ~330k refusals and ~225k demoted sites with the Pro window. Two new PC races
  under pmcjit (fmpc-arena, fmpc-arena2) were stopped by pc-run's video-memory watchdog (7.5 GB peak
  with CRAM off), so no PC instruction mix for the arena yet.
- The page-reuse pass (page_reuse_pass.inc) was off whenever the window was on, so every demoted or
  stack access did a full walk while the baseline grouped them. 1e844a1 groups stack and demoted
  sites with the window on (g2-learn: 118k groups, 612k accesses).
- Demotions halt only the faulting core now (08b973b, jit_impl.inc EdenDemote); other cores drop the
  block at their next Run boundary (its code stays until a full clear).
- Census 4987ee7 (piece_misaligned by area, piece_alias_failed): in g2-learn, 71,920 "partial"
  refusals of which **71,456 are stack-region (area 0x10) pieces whose backing is off a 16 KiB
  boundary**; 453 in the alias area, piece_alias_failed 3. Cause: MapMemory maps the stack onto the
  heap buffer's own pages, and the game's stack and buffer addresses differ modulo 16 KiB.
- Eden 0060 (d65b1f9): with the window on, such a MapMemory gets 16 KiB-congruent pages of its own
  (copied in; UnmapMemory copies back; the source is locked and not mapped for the guest meanwhile).
- 5f56244: dev settings fastmem64_stack=on (SP-based accesses take the window too; not grouped by
  page reuse then) and fastmem64_learn=on (the list is not loaded: other sessions' runs save the same
  list file between my delete and my race, so "learn" in fms.sh is unreliable).
- g2-learn was not a learning run (it loaded 155k sites another session saved): 47.81 fps (median
  51.79) on 1e844a1, the best race so far.

### Series g2 (1e844a1 page reuse + lazy demotion; from base1 on also Eden 0060)
| run | fps (median) | notes |
| --- | --- | --- |
| learn* | 47.81 (51.79) | *loaded 155k sites another session saved; 1e844a1 without 0060 |
| base1 | 44.38 (45.20) | fastmem64=off |
| use1 | 48.51 (51.48) | 0060: refusals 110 (was ~72k), faults 669 |
| base2 | 44.45 (45.49) | |
| use2 | 48.54 (49.62) | |
- Window +9.3% (48.5 vs 44.4); 5 clean stops, 0 GPU faults.

### Series g3 (c86d68f: list format EFS2, file <title>.fastmem64-sites2)
- learn (a real first session, no list): **30,876 sites demoted (was ~225k)**, 45.97 fps (median
  48.22) vs base1 45.63 (46.23). The first session no longer runs under the baseline.
- use1 (35k-site list): 47.43 (50.51). GPU thread busy 0.93: the frame now sits at the GPU thread.
- Learn census: blocked (read traps) 15.3k, blocked_write 9.2k, partial 6.7k (alias-area misaligned
  3.1k, scattered 1.8k, alias failed 1.2k); 729 regions in pieces against the old cap of 768.
- stk1 and the rest were refused by console-run.sh: the driver moved to Mesa f5610fe and builds with
  an older isolated RADV no longer run. Merged overnight-60fps (b4f1ae2) and rebuilt; series g4
  repeats the pairs with stack accesses on as a third arm.
- 3c80668: a region is shown in pieces while more than 1,024 CPU page tables are free (was a fixed 768).

### Series g4 (b4f1ae2: overnight-60fps 9329792 merged, Mesa f5610fe; FPS overlay on)
| run | fps (median) | gpu_busy | notes |
| --- | --- | --- | --- |
| base1 | 44.77 (47.22) | 0.82 | fastmem64=off |
| use1 | 46.02 (44.80) | 0.92 | window, 35k-site list |
| stk1 | 46.77 (48.41) | 0.92 | + fastmem64_stack=on: 2.4k faults, +1.8k sites, small_cap 0 |
| base2 | 43.23 (45.40) | 0.85 | |
| use2 | 45.80 (44.12) | 0.91 | |
- Window +4.3% on means (45.9 vs 44.0), stack-on 46.8 in one race; medians noisy. All clean stops,
  0 GPU faults; stk1/base1 frames at 380/410 s alike. With the window the GPU thread is ~92% busy.
- stk2 was lost (its console-run died while waiting, 06:05); stk2b and base3 queued behind the
  stage3 reservation (runs/reserved.for).

## 2026-11-04 morning (round 3 setup)
- lever-fastmem fast-forwarded to overnight-60fps 7504528 (round 2 merged; 0050-0053 never applied
  here); rebuilt, llvm-pie.elf has radv_ps5_retired_free. Copy of the elf: runs/g5-elf/.
- Pro reserved for stage3: queued g5 def1 (7504528 as merged), stk1, base1, def2, stk2, base2, then
  g5-share (pc_sample=on pc_core=0 pc_fast=on: fmshare.py, and native_pc.py for the cherry-picks).
- PC: wallprof races fmpc-prof1/2 stopped by the video-memory watchdog within a minute: ~4.9 GB
  were already in use (lever-gputrim's gt/fwab.sh replay loop shares the GPU).
- Cherry-pick 00285925f also fixes a bug present here: common/slot_vector.h begin() computes
  std::distance(it, stored_bitset.begin()) (reversed), so with slots 0-63 free iteration yields
  nothing; BufferCacheRuntime::TickFrame iterates slot_buffers every frame.

## 2026-11-04 late morning: the PC boot stall (lever-scout's stuck boots)
- Stuck boots (sc-pct4, sc-vkm0..3, 08:53-10:02): the darkened boot map stays on screen; presents stop at
  ~60-70 s (fence "not reached in 4 s"), the log goes quiet at 57-88 s, only 2 ASTC GPU decodes are
  logged in 9-300 s, no GPU-only region is ever created, low video memory (2.4-3.3 GB) and the run
  ends at its timeout (exit 124). Good boots log 4 decodes, then hundreds, back GPU-only regions from
  ~30-50 s and usually end at pc-run's video-memory watchdog (exit 143, 7.3-7.7 GB).
- Not reproduced in 10 boots here (runs/fmboot*), every one reaching the world load: my 0308762 build
  (5), with all 12 cores kept busy (1), with scout's shader/pipeline caches (1), with scout's vkmem
  layer (1), scout's own 2ded091 binary in my run folder (2), and scout's binary with scout's whole
  user folder (1). So neither the build nor anything in scout's run folder causes it: it was tied to
  that time window (other PC activity then; no NVRM errors in the kernel log).
- Tools for the next one: coordination/pc-bootdump.sh (boot with gdb thread dumps at 150/250 s),
  pc-run.sh PC_QUIET_DUMP=<s> (one dump of every thread when the game's log has been quiet that
  long), and headless/main.cpp (lever-fastmem): EDEN_HOST_STOP_DUMP makes the process attachable from
  the start. gdb is given the host binary and sysroot / (bwrap's /app path cannot be opened).

## Where I stopped (2026-11-04, pause until Saturday)
- Branch lever-fastmem tip ff4cc90 (on overnight-60fps 0308762): 8770222 (PC attach for PC_QUIET_DUMP) and
  ff4cc90 (a JIT fault at no fastmem site goes to the crash report) are not merged yet; both build.
- g5-def1 (7504528 as merged, window default) was on the Pro at the pause and ends by its own stop; the
  rest of g5 (stk1, base1, def2, stk2, base2, g5-share) was cancelled before starting.
- Next: read g5-def1 (fault count, frames); rerun the g5 pairs on the current overnight with ff4cc90
  (stack-on vs default vs base, interleaved), then g5-share (fmshare.py + native_pc.py with
  runs/g5-elf or a fresh elf) to measure the window share and the two cherry-picks (ddbd076cf/b2430b4d9
  shader-cache batches, 00285925f bitmap scans). Decide fastmem64_stack default from those pairs.
- Open: lever-stage3's s3a2r-on3 JIT fault (cause unknown; ff4cc90 will name the address next time);
  the PC boot stall (not reproduced; use PC_QUIET_DUMP=60); full PC races still end at pc-run's
  video-memory watchdog.
