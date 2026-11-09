# eden-nx2 review (2026-10-30)

Source: `git.eden-emu.dev/Takoroka/eden-nx2`, branch `eden-nx2` (tag `nx2-2026-10-29`, head 50fa86b),
read from a shallow clone. GPL-3.0-or-later like Eden. Its `NX2_NOTES.md` is a numbered log of 149
blockers with causes and fixes; "title 1" there is Mario Kart World (0400C3F00006E000, `Drive.nss`).

Where they are with MKW (Windows, RTX 4070 laptop): boots, title screen, cup-opening movie
(VP9 via FFmpeg), races. ~59 fps on a light course after the GPU-thread split; 20-30 ms per frame
on heavy courses. Still wrong: task/mesh-shader geometry is skipped (pop-in, low LODs), some
colour/texture issues on early builds.

Their tree is not ours: ProsperoEden's build g has its own services (IGpuWorker, IVi2Session,
ISyncpointSupervisor in `am/service`), theirs `nx2_*` (`nx2_gsv.h`). The video_core side shares
an ancestry ("changes.patch": `TuringMacroInterpreter`, native QMD scheduler, Ampere lowering), so
most video fixes port almost line for line.

## Fixes we lack, by how much they matter to us now

1. **Per-channel MME data RAM** (their row 84, e1cc47a). Our `NativeMacroContext` is per TSG and
   since our patch 0029 all four MKW queues share one TSG, so all channels share one data RAM
   (`TuringMacroInterpreter::dram`). MKW keeps per-channel state there (QMD ring base in word 0x27;
   our stuck macro at 0x143 reads/writes word 0x4bb). Their symptom: QMDs collide, releases early or
   never, GPU waits. Ours: GPU thread stuck in a draw emitted by macro 2 right after the first
   present (runs 21-26). Fix: macro code stays shared, each channel gets its own data RAM
   (`active_dram` pointer set per call), one QMD table for all channels. **Port first.**
2. **QMD releases** (rows 60-62): fire release slots 24, 28 and **52** (queue-to-queue syncs);
   QMD release slot 0 with flag bit 19 writes `max(old, payload)`; rejected QMDs still complete and
   release. **Releases latch waiters** (row 70, 8e37f58).
3. **Presentation** (rows 55/59/71/82): vi2 229 waits (bounded, up to 4 s) for the frame's GPU fence,
   else the game runs a whole command ring ahead and desyncs; vi2 31 and **32** return events
   signalled at 60 Hz (MKW loops at main+0x25ec5e4 until [obj+0x88] >= 2 on the 32 event; we
   answer 32 without a handle); 60 Hz vblank pacing.
4. **Memory** (rows 81/85/87/94/99/103/114): gpu 214 regions of ~5.7 GiB are sparse; images over
   2^28 texels created sparse and unbacked (MKW's 8192x8192x68 virtual texture, 23 GB decoded);
   huge ASTC arrays keep only levels that fit 512 MiB; alpha-less ASTC to BC1 (~70% of them) instead
   of BC3; GPU ASTC decode + BC encode in a compute pass; collector thresholds from the driver
   budget; 64 MiB VMA blocks. These address the texture memory our runs 8/9 ran out of.
5. **Mapping modify** (row 149): report characteristics flag bit 34
   (NVGPU_GPU_FLAGS_SUPPORT_MAPPING_MODIFY) and answer gpu 212 (NormalModifyMemoryKind4K) with the
   wanted kind; without it compressible textures (movie render targets) get GPU address 0.
6. **Syncpt** (row 147): 202 always signals the slot; 201 clears the slot's event first.
7. **Shaders** (rows 88-95, 112): 1699 of 1712 MKW programs lower (IPA modes, integer carry,
   uniform ops, surface load/store, jump tables past EXIT, 128-bit RZ stores, R2UR P6 save, MUFU
   SIN/COS take turns so scale by 2*pi, NOINC calls); negative cache for rejected programs.
   Missing everywhere: task/mesh (ISBERD/ISBEWR).
8. **Video** (rows 96/105/146/148): gsv 991/901/902/906/911/961/1001/1002 and an FFmpeg VP9 decoder
   service; needed for the cup-opening movie and race start.
9. **Performance** (rows 98-145): macro memo (97% of calls replayed), x86-64 MME JIT, GPU thread
   split in two (1.46x draws), no buffer downloads at fences for Switch 2, deferred inline-write
   invalidation, same-value register writes dropped, per-thread device-memory translation cache
   (fixes a crash), spinning cache locks, CNTPCT PAUSE, TSC measured on AMD. The PS5's Zen 2 cores
   make these matter more than on their PC.
10. Smaller: KeplerMemory bound on subchannel 2 by default; 4-byte semaphore releases write 4 bytes;
    channel suspend on unmet acquires (we have it); method data continues across GP entries (we have
    it); six emulated cores instead of folding 4/5 onto 0/1.

## Options

- Port item 1 now (small, likely our current stall), then 2-3, then memory (4) and 5-7.
- Longer term: rebase the Switch 2 side on eden-nx2. Cost: it is on a newer Eden master than our
  pinned 67bada77, so `tools/prepare-vulkan-port.py` anchors and the headless port patches need
  re-pinning.
