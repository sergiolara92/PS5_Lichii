# Command-stream JIT for the GPU thread (design + PC loop)

Goal: attack the one serial interpreter that bounds OOT (and every other title) on the console.
Measured on the PS5, mesh on: frame 68.4 ms (14.6 fps), of which `gpu_dispatch` covers the whole
frame, draw recording is 22.7 ms, host GPU wait 7.8 ms, and every guest core is idle 25-58 ms of
the frame. 60 fps needs 16.7 ms on that thread: a 4x cut, of which the decode side is the target.

## Where the work is (a42b4d4 + patches, verified 2026-11-06)

- `DmaPusher::Step` / `DispatchCalls` walk the push buffer; `ProcessCommands` decodes each
  `CommandHeader` (mode Increasing / NonIncreasing / Inline / IncreaseOnce, method, subchannel,
  arg count) and calls `CallMethod` / `CallMultiMethod` per word.
- `DmaPusher::CallMethod` → puller methods, else the subchannel engine:
  `execution_mask` / `method_sink`, `Nx2Profile::Scope`, then `Maxwell3D::CallMethod`.
- `Maxwell3D::CallMethod` → macro sanity check, `RecordWrite`, macro dispatch
  (`method >= MacroRegistersStart` → `ProcessMacro` → `FrontRunMacro` interpreter),
  `ProcessShadowRam`, skip-same, `ProcessMethodCall` (special methods + `draw_manager`), and
  finally `ProcessDirtyRegisters` (the plain register write + dirty flags).

## Plan

1. **Plan cache (decode once per entry).** Key an entry by its GPU address + size and validate by
   hashing the entry's words (a few hundred KB per frame). Decode into a flat op array
   (`Write`, `WriteRun`, `MultiMethod`, `Macro`, `Sync`, `Bind`) with the argument words as offsets
   into the entry, so nothing is copied. Execute with the dynamic state replayed exactly
   (`dma_state.method/subchannel/method_count/is_last_call`), calling the *same* entry points for
   every non-plain op. Correctness bar: identical draw count and identical frame digests versus the
   interpreter on the same replay.
2. **Folded engine writes.** Runs of plain Maxwell3D registers (not a special method in
   `ProcessMethodCall`'s switch, not a draw trigger in `DrawManager::ProcessMethodCall`, not a macro
   trigger, not puller) apply as `memcpy` into `regs.reg_array` plus a precomputed dirty mask,
   skipping `RecordWrite`/shadow-RAM/switch per word.
3. **Macro pre-expansion.** Compile each `FrontRunMacro` invocation into the op array once instead of
   interpreting the MME stream per draw.
4. **Fallback.** Any unmodelled op → run the rest of the entry through `ProcessCommands` unchanged.
   Dev setting `EDEN_NX2_CMD_JIT=1` selects the path, so interpreter and JIT are A/B on the same
   binary.

Expected: decode bucket ~45 ms → ~10-15 ms best case; the 22.7 ms draw recording is untouched, so
~26-30 fps is the ceiling of this lever alone. Not 60.

## PC development loop (state 2026-11-06)

- `make ns2-host` needs `~/.local/bin` on PATH (clang-18) and a `gmake` shim (`exec make "$@"`);
  with both, `build/ns2-host/eden-headless` builds.
- `coordination/pc-run.sh` no longer hardcodes the vanished `~/Downloads/ProdKeys...` paths: the
  keys folder, firmware folder and game default to `/home/mihawk/Desktop/Win-Games` (keys),
  `/home/mihawk/Desktop/Win-Games/firmware-empty` and the MKW `.xci`, each overridable with
  `PC_KEYS` / `PC_FIRMWARE` / `NS2_GAME`.
- `coordination/pc-replay.sh <tag>` with `CHECKOUT=.../ProsperoEden-oot` reaches playback start:
  capture opens, 2100 command lists in the window, shader cache loads, snapshot restores
  (5 channels, 1 address space).
- **Blocker:** the MKW capture window needs ~7.98 GB of the RTX 3070's 8 GB, so the VRAM watchdog
  stops the run before the playback results print. Next: trim the window (shorter capture) or lower
  the replay's texture-cache budget (`TC_EXPECTED` / `TC_CRITICAL` in pc-replay.sh, currently
  1536/2560 MiB), or capture a few frames instead of a full race window.

## Validation

Replay digest comparison (interpreter vs JIT) on PC for correctness; fps and per-draw time on the
console (OOT) for the speed claim, because PC timings do not transfer.

## Measurement 2026-11-06 (console, OOT, EDEN_NX2_CMD_STATS=1)

`DmaPusher::ProcessCommands` instrumented (entry count, words, wall time), 150 s run, mesh on,
80.1 ms median frame, 2,608 draws:

- 9,698,280 entries over the run (~64,655/s), 29.6 words per entry.
- **96.1 s inside `ProcessCommands`** of a ~150 s run: 64% of wall time.
- median 0.4 ms per 120 entries (3.3 us/entry), mean 1.19 ms (skewed by load bursts).
- Draws account for ~73 s of that (22.7 ms/frame x ~3,240 frames), so pure decode+dispatch
  overhead outside draws is **~22 s of the run, ~5-7 ms per frame (7-9%)**.

Conclusion: the command-stream JIT attacks ~7 ms of a ~71 ms frame. It is not the lever and should
not be built as planned (it is a multi-week project for ~10%). The frame is:
- draws (rasterizer_draw) 22.7 ms,
- decode/dispatch 5-7 ms,
- **~45 ms inside the dispatch that is neither**: the per-entry Vulkan work (`FlushCommands`,
  `OnCommandListEnd`, cache flushes/submits) and the waits around it.

Next target: name and cut that ~45 ms — batch `FlushCommands` across entries, avoid the per-draw
`FlushWork`, and count the per-entry submit cost. That is measured work, not a rewrite.

## Eglantine-PS5 (Vampinity) + viejunerd issue #21 — what it actually reports (2026-11-06)

Same layer stack as ours: Djihads `nx2` core pinned at `3081c8e` (we run a42b4d4 + 29 cherry-picks),
PS5 frontend from heydemoura/EglantinePS5 (ProsperoEden lineage), 14 PS5 patches of their own.

Their numbers, from their own writeup (`viejunerd/Eglantine-PS5-` issue #21):
- **OOT: 15-20 fps** (table: 11-21, 12-17), crashes from memory a couple of minutes in.
- Mario Kart World: 24-28 fps. So the "25 fps" is MKW, not OOT — we are at parity on OOT (14.6).
- Settings: Docked, Vulkan, 1x, Bilinear, **1080p**, 60 Hz, async shaders off, fast GPU on,
  reactive flushing off.
- Their OOM is fragmentation: 12224 MiB pool with 126 MiB free in 190 blocks, largest 4 MiB,
  and a 13 MB texture-upload request.

Their work we do not have and could take (their numbering, from the issue):
- `0069`/`0088` slice large decoded-image uploads; `0086` decodes ASTC one layer at a time
  (scratch 640 MiB -> 7.1 MiB).
- `0082`/`0083` (Lichii PR 104/103): download only Preflushable pages at fences; upload CPU writes
  onto GPU-written pages.
- `0089`/`0090`/`0095`-`0097`/`0130`-`0132`: a refused allocation no longer ends the session.
- Memory layout: 320 MiB flexible area with 512 KiB fiber stacks, no `cpuPageTableSize`,
  framebuffers at output size (~110 MiB at 1080p).
- `0081` (Orchid `052478de5b`) depth-compare descriptors stay float; `0087` invalid depth-stencil
  copies in ReinterpretImage (Lichii 89).
- `0061`/`0062` quarter descriptor queues, size kernel reference tables by region.
- `0041`/`0045`/`0046`/`0053` fence/semaphore/macro DMA on the split back thread (their
  GPU-thread `0x10` fault stopped). `0054` large ASTC on the CPU.

Conclusion for our tree: their fps advantage over us is not demonstrated for OOT; their advantage is
memory robustness (fragmentation + refusal handling), which is our original OOM problem. Next: run
our build at their settings (1080p output, fast GPU) for a fair A/B, and port the memory set above.
