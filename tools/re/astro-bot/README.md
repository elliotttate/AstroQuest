# ASTRO BOT Rescue Mission (CUSA12392): reverse-engineering notes

Notes from static analysis of the game's executable, written while working on the PC's frame
rate. Addresses are offsets in the main executable as loaded at 0 (the emulator adds the load
base). Nothing from the game itself is in this folder: only addresses, the analysis, and a few
short disassembly excerpts. To follow along you need your own dump of the game.

| File | What it is |
| --- | --- |
| [drawthread-sync.md](drawthread-sync.md) | How the game paces its frames in VR: DrawThread, Game:Main and Hmd::ReproThread, the 3 ms high-resolution timer armed after every vblank, the "ticks" a frame start waits for (two in the game's 60 fps mode, one in its 90/120 modes), and why Game:Main spends most of its time waiting on DrawThreadMuxtex (by design: DrawThread holds it through its whole frame, tick wait included). Background for `PRECISE_TIMERS` and the native 120 fps mode in [docs/pc-performance.md](../../../docs/pc-performance.md). |
| [native-rate.diff](native-rate.diff) | The emulator change that unlocks the game's own `FrameRate` option (60/90/120): the three places the retail build sets and holds it at 60, the bytes checked before patching, and how the emulated headset's refresh follows it (`SHADPS4_TITLE_NATIVE_RATE`). Kept as a reading aid; the change itself is in `shadps4-arm64-main/src/core/known_title.cpp` and `videoout/driver.cpp`. |
| [save-format.md](save-format.md) | The save (`sce_sdmemory/memory.dat`): plain XML at offset 0x400, no checksum. Per-level records `L1`..`L78` with `A` (unlocked 0/1), `B` (rescued bots bitmask), `C` (chameleon found), `D` (times cleared), `E` (best time); which `L` number is which level (`L1`-`L25` are the levels of Worlds 1 to 5); the unlock rules; what `/app0/args.txt` options do; where the planets are in the world select. |
| [make_save.py](make_save.py) | Builds edited saves for benchmarking more levels from the save `tools/pc-bench.sh` makes (`build/pc-bench/save`): `build/pc-bench/save-w2` (Worlds 1 and 2 cleared, World 3 opened) and `build/pc-bench/save-all` (Worlds 1 to 5 cleared). Run `python tools/re/astro-bot/make_save.py`, then e.g. `SAVE=build/pc-bench/save-w2 SCRIPT=<your input script> tools/pc-bench.sh <name>`. Saves stay in `build/` and are never committed. |
