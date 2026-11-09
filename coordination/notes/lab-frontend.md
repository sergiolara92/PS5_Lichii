# OOT critical chain is front-end bound (PS5 Linux lab, 2026-11-08)

Setup: base PS5 (Zen 2 Oberon, same CPU as the Pro, ~3.47 GHz with boost) running CachyOS; PC build of
ProsperoEden-gcache (patches to 0201) on RADV; OOT gameplay in Kokiri Forest (Link's house view), ~33.5 fps by
texture-cache frame ticks. Tools: coordination/lab/{labdrive.sh,lab-oot-load.sh,lab-frontend.sh} (`ssh ps5lab`).

## Counters, JIT code only (memfd:eden-jit), 12-15 s windows
| | cores 0-2 (critical chain) | cores 3-5 |
|---|---|---|
| IPC | 0.64-0.67 | 1.23-1.24 |
| dispatch starved (de_dis_uop_queue_empty_di0) | 44.4% of cycles | 29.9% |
| uops from op cache | 26% | 59% |
| ROB full (retire_token_stall, long-latency loads) | 8.4% | 7.0% |
| i-fills from L2 / from L3+DRAM per 1k insn | 49.8 / 14.2 | 21.9 / 5.0 |
| iTLB walks per 1k insn | 0.60 | 0.21 |
| DRAM data loads per 1k insn | 0.81 | 0.38 |
Cycles on cores 0-2: JIT 58.5%, PhysicalCore::Idle 28%, emulator ~9%, host kernel ~3%.

The IPC gap between the two core groups is code delivery, not data latency (ROB-full is the same ~7-8%).
Earlier PC (Intel) measurements (memory stalls 3.1%, TLB walks ~0.2%) could not show this: bigger, faster L3.

## Hot code footprint, cores 0-2 (cycles samples in the JIT code cache)
- 50% of JIT cycles: 9,300 64-byte lines = 581 KiB (> 512 KiB L2), on 1,005 4 KiB pages (4 MiB), 30 2 MiB regions.
- 90%: 8 MiB of lines (> the PS5's 4 MiB L3 per CCX), 41 MiB of pages.
- Inside the top-50% lines only 65% of the bytes are executed; pages are ~14% hot.
- dynarmic x64 emits each block's slow paths (deferred_emits) right after its hot code, and blocks are placed in
  first-compile order, so hot code is diluted and scattered. (Older dynarmic had a separate far-code region.)

## Levers (predictions, not built)
- A. Slow paths to a separate cold region (dynarmic's old far code): denser hot lines/pages. ~0.5-1.5 ms chain.
- B. Profile-guided hot-block re-emission into a compact hot region (recompile hot blocks contiguously, ordered by
  execution; tiering hook from 0182 exists): the 581 KiB becomes contiguous; iTLB, op cache and L2 prefetch improve.
  If cores 0-2 reach cores 3-5's front-end profile (starved 44% -> 30%): JIT time -20%, chain -16%, ~3-4 ms of the
  Pro's ~23 ms heavy-frame chain (range 2-5 ms). Includes A's gain.
- C. 2 MiB pages for the code cache: iTLB walks 0.6/1k insn, ~1-2% of cycles. Small alone; free with B.
Check on the Pro: same Zen 2 caches; the PS5 OS page size and code-cache mapping differ from Linux.

## Lever A built: patch 0202 (EDEN_JIT_COLD=1, slow paths into 256 KiB cold chunks), lab ABBA 2026-11-08
coordination/lab/lab-oot-ab.sh, OOT house view, same binary, off/on/off/on:
| | off | on |
|---|---|---|
| fps | 33.13 / 33.20 | 35.12 / 35.63 (+6.7%, 30.2 -> 28.3 ms per frame) |
| JIT IPC cores 0-2 | 0.65 / 0.66 | 0.73 / 0.73 |
| dispatch starved | 44.1% | 37.7-38.7% |
| op cache share | 26% | 27% (unchanged: the gain is L1I/L2/iTLB) |
| 50% of JIT time in | 568-579 KiB, 1008-1014 pages | 407-419 KiB, 652-676 pages |
| i-fills from L3/DRAM per 1k insn | 14.4 | 11.7-12.2 |
Above the prediction (0.5-1.5 ms chain). Pro confirmation pending. The switch is read at the first block
(console dev-settings env=EDEN_JIT_COLD=1 is applied after static init).

## Pro confirmation and default (2026-11-08)
Pro OOT ABBA (oot-console-run.sh, house view pinned at 30.0 fps by pacing, so busy time is the measure:
coordination/core-busy.py): cores 0-2 busy off 67.0/65.1% -> on 61.1/61.4% (-7% CPU time), core 3 67.4/63.5 ->
57.5/57.9%. Four clean runs, no JIT pressure. 0202 is now on by default (EDEN_JIT_COLD=0 turns it off); the launcher
tester build with it is installed on the Pro.
Door test (lab-oot-door.sh: walk out of Link's house, open the door, frames 60-90 s later): off MOVING, on MOVING x2.
One earlier on-run stuck after the door while sticks were pushed during the transition and the lab was 2.7 GB into
swap; not reproduced.

## Lever B mechanism (EDEN_JIT_CLEAR_FILE: one JIT group clear in gameplay, code recompiled in run order)
Lab, cold on: fps 35.54 -> 36.09, JIT IPC 0.72 -> 0.74, starved 38.3 -> 34.8%, 50% of JIT time 420 -> 344 KiB,
~670 -> 492 pages. Op cache share stays 26%: the hot code (344 KiB) is far bigger than the op cache, so the rest of
the front-end cost needs smaller code (fewer host instructions per guest instruction), not layout. Lever B
downgraded to ~1-3%.

## Fork review (2026-11-08): no new JIT codegen change in Eglantine rc3/nx2-experimental, Fritchii v4.1, Lichii 117
Candidates, all small: Eglantine 00285925f (priority-queue next-set-bit, kernel scheduler path), Fritchii EXP14
9b8b6fdd8 (drop same-value FrontWrite), Lichii a195b27ff (unroll software HMMA; matters if the GPU limits at 60).

## Open world (user save in Hyrule Castle Town, 2026-11-08 evening)
- OOT asks for 60 fps (no "present interval" change in any log); the Pro's steady 30 in the house is vblank quantization.
- Pro, Castle Town -> Hyrule Field (scratchpad pro-field.sh, oot-console-run.sh OOT_ROUTE/OOT_MOVE): with the split GPU
  thread ON it deadlocks at the transition (channels 1/2/3/5 wait on semaphores, fps 0, OOT aborts 0x1a80a after ~3.5 min),
  2/2 runs, 0200 on or off. Split OFF passes: field 14.8 fps avg / 14.9 median (~67 ms), 3,097 draws/frame, front 75% busy.
- Lab (base PS5): our picture lacks foliage/props, is washed out, no shadows; log "Draw skipped (no VK_EXT_mesh_shader)"
  (gfx1013 has no mesh shaders under RADV). Lichii build 117 draws them (compute fallback b7ff181f2 #569). 0200 off: no change.
  The Pro (NAVI21-class) skips no mesh draws. Lab memory (14 GB) needs the console's CRAM settings (labdrive now sets them).
- Lab intermittent crash at area loads: assert index <= tic_limit (shader_environment.cpp ReadTextureInfo), then SIGSEGV;
  seen with 0202 off too. Core dumps now enabled on the lab (labdrive ulimit -c).
- Fork comparison on the lab: Lichii 117 ~14-15 fps in Castle Town but 8 GB in swap (no CRAM): not a fair number yet.
