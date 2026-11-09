# ProsperoEden-heap: live-heap census and first cut (2026-10-31), paused

State: finished, nothing half-done, no build or run of mine on the console or the PC.
- Committed locally on eglantine-heap (not pushed): cd25ec9 "VFS: drop the open-file cache's
  entry when its file is gone" (headless/CMakeLists.txt, derived vfs_real.cpp).
- Console, base a63dfee: game_loaded live 706 -> 579 MiB (2.41 M -> 1.17 M blocks); race in_use
  median 1638/1628 -> 1566 MiB; race held not lower (noise); race fps 22.7/24.8 -> 23.9.
  Runs: runs/heap-vfs-base (pool OOM in the race; heap log = crash-20261031-150015-heap.log),
  runs/heap-trim4 (same code, earlier), runs/heap-vfs-fix (ran to the end).
- The ranked census and the next cuts are in notes/ProsperoEden-heap.md ("Live heap census by call
  site", "Next cuts by size"). Tools: heapsite/heapsite2.c, rank.py, heapcmp.py.

Next if resumed: RomFS built twice (~65 MiB); A64 fast_dispatch_table 16 MiB x 6 cores (perf check
needed); guest descriptor queue 96 MiB (measure the per-frame high water first).
