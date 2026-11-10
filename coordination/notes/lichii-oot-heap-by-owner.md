# OoT heap by owner (PS5_Lichii, PC build, 2026-11-10)

Measured with `coordination/heapsite/heapsite2.c` (LD_PRELOAD) on `build/ns2-host/eden-headless`, the Pro save
loaded into Link's house, then a walk out into Kokiri. Dumps and rankings: `coordination/runs/heapsite-oot-20261110/`
(`hs-run.sh` is the driver; rank with `coordination/heapsite/rank.py <binary> <dump> 60 -v`).

Live heap: 1,180 MiB at the title, 2,360 MiB at the peak of the save load (240 s), 1,710 in the house,
1,735-1,740 after the walk (flat for 2 minutes).

| MiB in gameplay | Owner |
|---:|---|
| 367 | GPU tables: descriptor update queue 120, GPU page tables 67.5, TIC 54, TSC 54, Tegra::GPU 25, device memory manager 16, buffer cache pages 15, GPU split 8, scheduler chunks 8 |
| 279 | Game files: decoded NUP members 165 (PC cap 512; the console caps this at 64), NUP/RomFS tables 114 in 1.06 M small blocks |
| 224 | GPU driver (NVIDIA on the PC; RADV on the console is not measured by this) |
| 214 | CPU JIT: block map 97 in 743,000 blocks, fast dispatch tables 96 (6 x 16), block ranges 21 |
| 189 | Kernel: fiber stacks 81.5 (163 x 512 KiB), dynamic page manager 72, memory manager 35 |
| 142 | Texture scratch kept after one large upload: upload buffer 64, join buffers 62, DMA copies 16 |
| 113 | Shader IR pools kept by the translate workers (blocks 68, instructions 45) |
| 47 | Other shader and pipeline data |
| 34 | Fonts |
| 126 | System object, services, sites under 256 KiB |

The load peak is the GPU driver: 930 MiB at 240 s against 224 in gameplay (640 of it one compiler arena of
1 MiB blocks). The emulator's own data is 1,430 at the peak and 1,510 in gameplay: it does not spike.

Not tracked: mmap'd memory (guest RAM, JIT code, sparse tables). glibc malloc, not the PS5 heap arenas.

Cuts made (patch 0230, same day): upload scratch buffers freed every 256 frames when over 8 MiB and the ASTC
scan buffer after a scan over 8 MiB; a shader IR pool keeps at most 4 MiB between shaders; the descriptor
queues start at an eighth (12 + 3 MiB) and grow on demand up to the old ceilings. Measured again on the PC
build in Link's house (dumps coordination/runs/heapsite-oot-20261110-0230): live 1,710 -> 1,445 MiB.
Descriptor queue 120 -> 15, shader pools 113 -> 24, texture scratch 120 -> 7.5. No "Descriptor payload grew"
line: 12 MiB was enough there. Not measured: Kokiri (the walk did not leave the house on this run), the Pro.

On the Pro (run hp, 2026-11-10, with 0229 and the buffer collector change; reference run pt): heap live peak
2,574 -> 1,421 MiB, heap held at the crash 2,428 -> 1,879 MiB (crash reports). The descriptor queue grew twice at
the save load (65,536 -> 131,072 entries, 24 MiB; compute 16,384 -> 32,768, 6 MiB) without a fault. The pool
still ran out, now at the Hyrule Field transition (pt took a 13.9 s frame there and died ~100 s later with
std::bad_alloc): a packed guest slot found no 2 MiB block to unpack into, SIGSEGV on CPUCore_4, guest held
5,786 MiB, so ~5,970 MiB was graphics and untracked. Frame rate in the first area unchanged (~20.5 fps).
Logs: coordination/runs/hp (crash-20261110-103400-*).

Cuts left, by size: fast dispatch tables 0x100000 -> 0x40000 (72, needs a CPU check), TIC/TSC sized to the
guest's limit (up to 108), RomFS build tables freed after the build (~50).
