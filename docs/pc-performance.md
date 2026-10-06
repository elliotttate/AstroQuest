# PC performance: how ASTRO BOT got to 120 frames a second

AstroQuest plays ASTRO BOT Rescue Mission (CUSA12392) in a PC VR headset: a fork of the shadPS4
PlayStation 4 emulator (`shadps4-arm64-main/`) plus a launcher for PC VR (`pc-vr/`, see
[README-PC-VR.md](../README-PC-VR.md)). This page describes the performance work on the PC
build: what limited the frame rate, what was changed, how each change can be switched off, what
was measured, and what is still open. The first part is for players; the rest is for anyone
who wants to check, reproduce or continue the work.

All numbers on this page were measured. Nothing here is an estimate unless it says so.

## Contents

- [In short, for players](#in-short-for-players)
- [How the numbers were measured](#how-the-numbers-were-measured)
- [Earlier work (v0.13-perf1 and v0.13-perf2)](#earlier-work-v013-perf1-and-v013-perf2)
- [1. Why the frame rate stopped near 100](#1-why-the-frame-rate-stopped-near-100)
- [2. Precise HR timers (PRECISE_TIMERS)](#2-precise-hr-timers-precise_timers)
- [3. Precise sleeps (PRECISE_SLEEP)](#3-precise-sleeps-precise_sleep)
- [4. Stream copy memo (STREAM_MEMO) and the guest write epoch](#4-stream-copy-memo-stream_memo-and-the-guest-write-epoch)
- [5. Result: uncapped](#5-result-uncapped)
- [6. The game's own 120 fps mode](#6-the-games-own-120-fps-mode)
- [7. Present thread: 32 looks a refresh](#7-present-thread-32-looks-a-refresh)
- [8. Delivery phase](#8-delivery-phase)
- [9. Result: paced at 120 Hz](#9-result-paced-at-120-hz)
- [10. Measurement tools](#10-measurement-tools)
- [11. Open findings](#11-open-findings)
- [12. Reproducing the measurements](#12-reproducing-the-measurements)
- [Reference: environment variables](#reference-environment-variables)
- [Reference: where the code is](#reference-where-the-code-is)

## In short, for players

- **`fps=120` in `pc-vr\settings.txt` now uses the game's own 120 frames a second mode.** The
  game has a 60/90/120 fps option that the retail build locks to 60. The emulator unlocks it.
  At 120 the game simulates and renders a new frame for every refresh of the headset, instead
  of rendering 60 and having each one shown twice.
- On the test PC (i9-13900KF, RTX 5090) the first level runs at **119.8 frames a second**, and
  **about 99.4% of the pictures the headset shows are new frames** (0.75 repeated pictures a
  second). Before this work the same PC managed about 100.
- Set Virtual Desktop to 120 Hz (Streaming > Frame rate) to use `fps=120`.
- `native_rate=0` in `settings.txt` turns the native mode off. The game then stays in its 60 fps
  mode and is driven twice as fast instead (about 112 frames a second on the test PC, with
  more uneven frame times).
- The default is still `fps=60`, the console's own rate. Above 60, the few things the game
  counts in frames (some animated signs) run faster.
- The game's 90 fps mode is supported by the patch but has not been measured yet. The launcher
  only turns the native mode on for `fps=120`.
- At 120 fps the RTX 5090 is only 28 to 35% busy. What limits the frame rate is the emulator's
  processor work, not the graphics card.
- Every change on this page can be switched off with an `env=NAME=value` line in
  `settings.txt` (see [Reference: environment variables](#reference-environment-variables)). If
  something looks wrong after an update, `env=SHADPS4_PERF_ALL=0` turns all the switchable
  emulator optimizations off at once.

## How the numbers were measured

**Test PC:** Intel i9-13900KF, NVIDIA RTX 5090, Windows 11.

**Scene:** `tools/pc-bench.sh` starts the game from a save, plays a scripted walk
(`tools/bench/level1-walk.txt`) into the first level of World 1 (Rooftops), and sums up the
emulator's `SHADPS4_BENCH` lines. A run lasts 170 s and the level is reached at about 75 s. Only
the 10-second windows from the 8th on (80 s and later) are counted, so the numbers are for the
part inside the level. Each eye is drawn at 2880x3072.

**Headset:** the OpenXR Simulator stands in for the headset. It runs at 90 Hz by default; the
build used here adds `OPENXR_SIM_REFRESH_HZ` to run it at 120 Hz.

**Two ways to run:**

- **Uncapped** (`SHADPS4_VR_UNCAPPED=250`, the default of `pc-bench.sh`): the emulated headset
  refreshes by its own clock at 250 Hz, whatever the simulator's display does. This shows what
  the PC can do rather than what a display allows. In the game's 60 fps mode a frame takes two
  refreshes, so 125 fps is the ceiling.
- **Paced** (`UNCAPPED=0`): the emulated headset follows the simulator's display, as when
  playing.

**What the columns mean:**

| Column | Meaning |
| --- | --- |
| fps | The game's own frames a second: calls to `sceGnmSubmitDone`. A repeated picture is not a frame. |
| p99 | The 99th-percentile frame time (ms) of the slowest 10-second window. |
| 1% low | 1000 / p99, as frames a second. |
| GPU thread | CPU milliseconds a frame used by the emulator's GPU command thread. |
| emulator | CPU milliseconds a frame used by the whole emulator process. |
| shown | New frames the headset displayed a second (from the emulator's "Headset:" log line). |
| repeated pictures | Headset pictures that showed the same frame as the picture before, a second (from `SHADPS4_XR_FRAME_LOG`, see [10](#10-measurement-tools)). |

## Earlier work (v0.13-perf1 and v0.13-perf2)

Two experimental releases on [github.com/elliotttate/AstroQuest](https://github.com/elliotttate/AstroQuest)
came first:

- **[v0.13-perf1](https://github.com/elliotttate/AstroQuest/releases/tag/v0.13-perf1)** (commit
  5bec2b4): less work per draw on the GPU command thread, with ideas ported from or measured
  against the [GR2fork](https://github.com/junminlee2004/GR2fork) shadPS4 fork:
  - `IMAGE_FAST_STATE`: texture, render-target and depth bindings skip the texture cache's
    lock when the image is clean, tracked and already touched in this GC tick.
  - `FINDIMAGE_LOCKFREE`: repeated image lookups are answered without the global texture
    cache lock.
  - `SPEC_MEMO`: a per-program memo from the shader specialization inputs to the permutation,
    so a draw no longer builds and compares specializations.
  - `LOG_LEVEL_FIRST`: log macros check the level before formatting anything.
  - `DUMP_CHECK`: the per-draw "is a register dump requested" check is an atomic flag.
  - Always on: no per-draw copy of the fetch shader data, no copies of memory-area records on
    every GPU fence write, cached thread names.
  - Pipeline cache fixes, and the measurement tools `SHADPS4_BENCH`, `SHADPS4_PROFILE` and
    `SHADPS4_STACKS`.
- **[v0.13-perf2](https://github.com/elliotttate/AstroQuest/releases/tag/v0.13-perf2)** (commit
  b103b84):
  - `VIEW_MEMO`: each image remembers the view it handed out last.
  - Fixes merged from the [evertec82 fork](https://github.com/evertec82/AstroQuest).
  - `runtime=` in `settings.txt` picks the OpenXR runtime.
- **After perf2** (commit 0e1fa0e, not in a release): the uncapped benchmark mode
  (`SHADPS4_VR_UNCAPPED`) and `tools/pc-bench.sh`.

These made the GPU command thread cheaper: from 7.87 to 7.27 ms of CPU a frame (v0.13-perf2 with
and without `SHADPS4_PERF_ALL=0`, below). But the frame rate stayed at about 99 either way. The
limit was somewhere else.

## 1. Why the frame rate stopped near 100

**Problem.** Uncapped, with room for 125 frames a second, the game ran at about 99. Making the
GPU command thread cheaper did not change that.

**Cause: how the game paces its frames.** Static analysis of the game's code is in
[tools/re/astro-bot/drawthread-sync.md](../tools/re/astro-bot/drawthread-sync.md). In short:

```
emulated vblank ──> Hmd::ReproThread arms a 3 ms HR timer (sceKernelAddHRTimerEvent)
                       │ 3 ms later
                       ▼
                    timer fires: tick count + 1, wake DrawThread
                       │
DrawThread's frame start waits for the tick count to reach its target:
   game's 60 fps mode: 2 ticks a frame        90 / 120 fps modes: 1 tick a frame
                       │
                       ▼
DrawThread submits the frame, releases DrawThreadMuxtex
   ──> Game:Main hands over the next frame ──> DrawThread prepares it ──> waits for ticks again
```

- What starts a frame is the 3 ms timer tick after a vblank. It is not the vblank itself, a
  flip, or GPU completion.
- DrawThread holds the game's `DrawThreadMuxtex` mutex (the game's own spelling) for its whole
  frame, including the wait for the tick. So the game's Game:Main thread spends 63 to 72% of
  its time waiting for that mutex (measured with `SHADPS4_MUTEX_STATS`, see
  [10](#10-measurement-tools)). This is how the game is built, not an emulator bug, and not
  the limit: Main hands over the next frame as soon as DrawThread is done.
- To get past 60 in the game's 60 mode, the emulated headset has to refresh at 240 or 250 Hz.
  A 3 ms timer after each vblank then leaves only about 1 ms before the next vblank. On the
  console, at 120 Hz, it leaves 5.3 ms.

**Why ticks were lost.** The emulator ran the game's HR timers on `boost::asio` steady timers.
On Windows those wait with the ordinary system timer, which fires up to about 1.5 ms late. A
timer that fired after the next vblank had already re-armed it was merged with the new one, so
one tick was lost. The frame then took three refreshes instead of two: 12.5 ms at 240 Hz
instead of 8.3 ms. Enough of those, and 125 possible frames a second become 100.

The fix has two parts:

- Sections 2 and 3 make the emulator keep the game's timing, and section 4 takes work off the
  GPU command thread. Together they reach 119.6 frames a second uncapped (section 5).
- Section 6 switches the game to its own 120 mode, which needs one tick a frame and has a whole
  refresh of margin for it.

## 2. Precise HR timers (PRECISE_TIMERS)

**Where:** `shadps4-arm64-main/src/core/libraries/kernel/equeue.cpp` (`PreciseTimerThread`,
`EqueueInternal::SchedulePreciseTimer`, `TriggerPreciseTimer`).

**What it does.**

- HR timers shorter than 1.2 ms were already spun on by the existing "small timer" path. Those
  of 1.2 ms or more, the game's 3 ms tick among them, used to go to asio. They now go to a
  thread of their own, `shadPS4:PreciseTimers`, at high priority (`THREAD_PRIORITY_HIGHEST`).
- That thread keeps a queue ordered by deadline. It sleeps on a Windows high-resolution
  waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) until shortly before the earliest
  deadline, then spins (`_mm_pause`) for the rest. A newly armed timer that is due earlier
  wakes it.
- The spin lasts `SHADPS4_TIMER_SPIN_US` microseconds (default 300, range 0 to 5000).
- Every arming of a timer gets a serial number. When a timer is deleted and armed again before
  its old deadline, the old firing still reaches the queue, but it carries the old serial and
  is ignored. Only the current arming triggers the event.
- Ordinary (non-HR) kernel timers are unchanged.
- On platforms other than Windows the thread waits on a condition variable, and yields in the
  last stretch instead of pausing. Only the Windows build was measured.

**Effect.** HR timers now fire about 70 µs late on average, 0.3 to 1 ms at most. The `Bench:`
line (`SHADPS4_BENCH`) now ends with
`HR timers N a frame, late avg X us max Y us`, measured for both paths, so the two can be
compared.

**Switch:** `SHADPS4_PERF_PRECISE_TIMERS=0` goes back to the asio timers.

## 3. Precise sleeps (PRECISE_SLEEP)

**Where:** `shadps4-arm64-main/src/common/thread.cpp` (`AccurateSleep`, Windows).

**What it does.** `AccurateSleep` is used by the game's `sceKernelUsleep` and `nanosleep`, by
`AccurateTimer` (which paces the present thread's looks, see [7](#7-present-thread-32-looks-a-refresh))
and by audio output. It used to create an ordinary waitable timer for every sleep, wait on it,
and close it again. An ordinary timer wakes up to about a millisecond and a half late. It now
keeps one high-resolution waitable timer per thread, made once.

**Switch:** `SHADPS4_PERF_PRECISE_SLEEP=0`.

## 4. Stream copy memo (STREAM_MEMO) and the guest write epoch

**Where:** `shadps4-arm64-main/src/video_core/buffer_cache/buffer_cache.cpp`
(`BufferCache::ObtainBuffer`), `buffer.h`/`buffer.cpp` (`StreamBuffer::Wraps`),
`src/common/perf_toggles.h` (`Common::GuestWriteEpoch`).

**Problem.** Small read-only guest buffers (vertex, index and constant data that the GPU has
not written) are copied into a "stream buffer" every time a draw binds them. Each eye's passes
and the shadow pass bind the same data again and again, so the same bytes are copied many
times a frame.

**Fix.** A 4096-entry table remembers where a copy of each (address, size) went in the stream
buffer. A later bind of the same address and size reuses that copy instead of making a new
one, as long as nothing can have changed the source or overwritten the copy. All of these must
still be the same as when the copy was made:

- the command buffer being recorded (its tick); when it ends, the stream buffer's space is
  freed;
- the stream buffer's wrap count; once it wraps to its start, earlier space can be handed out
  again;
- the guest write epoch, below.

When any of them changes, every entry is invalidated at once (a "scope" counter moves on). The
memo is used only on the GPU command thread.

**The guest write epoch** (`Common::NoteGuestWrite` / `GuestWriteEpoch`) is a counter that moves
on whenever guest memory may have changed under the GPU command thread:

- packets that write or copy memory, or wait for someone else's write: `EVENT_WRITE_EOP`,
  `EVENT_WRITE_EOS`, `RELEASE_MEM`, `DMA_DATA`, `WRITE_DATA`, `COPY_DATA`, `DUMP_CONST_RAM`,
  `MEM_SEMAPHORE`, `WAIT_REG_MEM`, `REWIND`, `STRMOUT_BUFFER_UPDATE`;
- the start of every graphics and compute command list, and every point where the command
  processor yields inside one (the title or another queue may write meanwhile);
- buffer fills and copies (`BufferCache::FillBuffer`, `CopyBuffer`), and
  `MemoryManager::TryWriteBacking`.

For developers: the memo assumes that the title does not rewrite a buffer with its CPU, while a
command list that already used that buffer is still being processed, without one of these
synchronization points in between. If a title ever renders wrong with this on,
`SHADPS4_PERF_STREAM_MEMO=0` is the first thing to try.

**Effect.** In level 1 the memo answers about 6,300 to 6,700 copies a frame, so about 6.4 to
6.7 MB a frame is not copied. About 7,300 copies (7.3 MB) a frame are still made. The `Bench:`
line shows `stream memo answered N a frame (K KB a frame not copied)`.

**Also changed:** the `Bench:` line's analysis of repeated stream copies (within a command list
and within a command buffer) is now opt-in with `SHADPS4_BENCH_STREAM=1`. It hashed every copy
and cost about 9% of the GPU thread's time, even when only measuring.

**Switch:** `SHADPS4_PERF_STREAM_MEMO=0`.

## 5. Result: uncapped

Level 1, emulated headset free-running at 250 Hz (at most 125 fps in the game's 60 mode),
OpenXR Simulator showing the frames:

| Build | fps | p99 ms | 1% low | GPU thread ms/frame | emulator ms/frame |
| --- | --- | --- | --- | --- | --- |
| v0.13-perf2 | 99.2 | | | 7.27 | 24.6 |
| v0.13-perf2 with `SHADPS4_PERF_ALL=0` (its optimizations off) | 99.5 | | | 7.87 | 24.6 |
| precise timers + precise sleep + stream memo | **119.6** | 13.15 | 76.0 | **5.72** | **20.8** |

The game got from about 99 to 119.6 frames a second. At the same time the GPU command thread
used 1.55 ms less CPU per frame, and the whole emulator 3.8 ms less, even though there were
20% more frames.

## 6. The game's own 120 fps mode

**Where:** `shadps4-arm64-main/src/core/known_title.cpp` (`OnGameLoaded`, `OnFrameSubmitted`,
`HeadsetHalves`), `src/core/libraries/videoout/driver.cpp` (present thread),
`pc-vr/launch.ps1`. Analysis in [tools/re/astro-bot/](../tools/re/astro-bot/README.md).

**The option.** The game's engine has a "FrameRate" option with the values 60, 90 and 120. The
retail build locks it to 60 in three places, all in the main executable (offsets as loaded at
0):

| Offset | Instruction | What it does |
| --- | --- | --- |
| 0xbe3689 | imm64 of `movabs rax, 0x3c0000003c` at 0xbe3687 | The display manager creates the option with 60 as value and as default. |
| 0xbe4fae | imm8 of `cmp dword [rbx+0x90], 0x3c` at 0xbe4fa8 | Its Initialize checks for 60... |
| 0xbe4fb5 | imm32 of `mov esi, 0x3c` at 0xbe4fb4 | ...and sets it back to 60. |

**The patch.** With `SHADPS4_TITLE_NATIVE_RATE` set, the emulator checks, as the game loads,
that all three places hold exactly the expected bytes, then writes the new rate into them.
`90` gives 90; `1`, `120` or any other positive number gives 120; `0` or unset leaves the game
alone. If the bytes differ (another version of the game), nothing is patched. The log then
says "The title's frame-rate option is not where it was expected: it keeps its 60 frames a
second".

**What the game does in 120.**

- When the headset output starts, the game copies the option into its `PresentRate`
  (0x2e460c4).
- Its frame start (0xc40d40) then waits for one tick per frame instead of two, so it starts a
  new frame at every refresh.
- Its engine steps the simulation every frame.

**What the emulator does to match.**

- After every frame, the emulator reads `PresentRate` to learn how many refreshes of its
  headset the game takes for a frame: two at 60, one at 90 or 120.
- `Core::KnownTitle::HeadsetHalves()` turns that into how long one refresh of the emulated
  headset lasts, in half refreshes of the real display.
  - With `fps=120` in the 60 mode, the emulated headset refreshes at 240 Hz (two refreshes a
    frame).
  - In the native 120 mode, it refreshes once per frame, at 120 Hz, like the real display.
    The 3 ms tick then has a whole 8.3 ms refresh to fall into instead of half of one.
- The present thread uses `HeadsetHalves()` to decide when the emulated headset refreshes.
  The delivery phase lock (section 8) keeps using `FramePace()`, the display refreshes per
  frame.

**Launcher.** `pc-vr/launch.ps1` sets `SHADPS4_TITLE_NATIVE_RATE=120` whenever `fps=120`, unless
`settings.txt` has `native_rate=0`.

**Is it reprojection? No.** In its 60 mode the game renders 60 frames a second, and the PS VR
reprojection shows each one twice, re-aimed for the newest head position. In the 120 mode every
picture is a newly simulated and newly rendered frame. The logs show this:

- The game's clock line reads "frames take 8.3 ms, its time step is 8.3 ms, the game ran at
  100% of its speed".
- Every frame is a full render: about 50 render passes and about 1,860 draws.
- The frame ids the headset receives go up by one per picture (`SHADPS4_XR_FRAME_LOG`).
- Screenshots taken at the same moments of the scripted walk show the same positions in the
  60 and 120 modes, so the game runs at the right speed.

A note on the clock line, for developers: by default the emulator sets the game's time step to
what its frames really take (`real_time` in `settings.txt`, `SHADPS4_TITLE_TIMESTEP=0` turns
that off). That is what keeps the game at 100% of its speed at any frame rate. The same line's
"(it would have at 200% left to itself)" is computed against the 60 fps step the game was made
for, so it does not describe the native 120 mode. According to the static analysis, the engine
sets its own step from the FrameRate option there (0xbe6b2c).

**Not yet done:** the 90 setting is supported by the patch but has not been measured.

## 7. Present thread: 32 looks a refresh

**Where:** `shadps4-arm64-main/src/core/libraries/videoout/driver.cpp`
(`VideoOutDriver::PresentThread`).

**Problem.** The present thread does two jobs:

- it hands frames the GPU has finished to the headset ("early flip");
- it decides when the emulated headset's vblank is due.

It does both at a fixed number of "looks" per refresh, paced by `AccurateTimer`. At 8 looks,
the looks were about 1 ms apart at 120 Hz. A finished frame could wait up to a millisecond
before it was handed over, and the vblank signals came up to a look late. That put the game's
frames around the moment the host takes its picture.

**Fix.** 32 looks a refresh by default, 0.26 ms apart at 120 Hz. `SHADPS4_PRESENT_LOOKS=<n>`
sets another number (2 to 64). Uncapped benchmark runs still use 2, as before.

**Effect at 120 Hz in the native mode:**

| | p99 ms | 1% low | shown/s |
| --- | --- | --- | --- |
| 8 looks | 10.68 | 93.6 | 117.8 |
| 32 looks | 9.80 | 102.0 | 118.8 |

Both runs used the old delivery phase, 0.55 (section 8). The 32-look run had 1.23 repeated
pictures a second in the level. The 8-look run also had `SHADPS4_MUTEX_STATS=10` on.

## 8. Delivery phase

**Where:** `HeadsetRefreshClock::NoteDelivery` in `videoout/driver.cpp`.

**What it is.** When the host (Virtual Desktop, the simulator) says when its display
refreshes, the emulated headset follows it with a phase lock:

- for every frame that comes at the game's own pace, the emulator notes where in the host's
  refresh it arrived;
- it then slowly moves the emulated headset's refreshes (gain 0.06) until frames arrive at a
  chosen point of the host's refresh.

That point, as a fraction of the host's refresh, is now `SHADPS4_VR_DELIVERY_PHASE`: default
**0.35**, range 0.1 to 0.9. It was a fixed 0.55. An earlier point leaves more room for a frame
that takes a little longer than the last, before the host takes its picture.

Every 10 s the log reports the headset's delay after the host's display, how many frames came
at the game's pace, and where in the refresh they arrived (`The headset refreshes X ms after the
host's display ...`).

**Effect at 120 Hz in the native mode, with 32 looks:**

| Phase | p99 ms | 1% low | shown/s | repeated pictures/s |
| --- | --- | --- | --- | --- |
| 0.55 (before) | 9.80 | 102.0 | 118.8 | 1.23 |
| 0.30 | 9.39 | 106.5 | 119.3 | 0.75 |
| 0.40 | 9.46 | 105.7 | 119.3 | 0.75 |

The new default, 0.35, lies between the two best values measured.

## 9. Result: paced at 120 Hz

OpenXR Simulator at 120 Hz (`OPENXR_SIM_REFRESH_HZ=120`), `fps=120`, eyes 2880x3072, level 1:

| Configuration | fps | p99 ms | 1% low | shown/s | repeated pictures/s |
| --- | --- | --- | --- | --- | --- |
| Game's 60 mode, emulated headset at 240 Hz | 112.3 | 15.45 | 64.7 | 103.4 | |
| Native 120 mode (8 looks, phase 0.55) | 119.8 | 10.68 | 93.6 | 117.8 | |
| Native 120, 32 looks (phase 0.55) | 119.9 | 9.80 | 102.0 | 118.8 | 1.23 |
| Native 120, 32 looks, phase 0.30 to 0.40 (default now 0.35) | **119.8** | **9.39 to 9.46** | **105.7 to 106.5** | **119.3** | **0.75** |

With 0.75 repeated pictures a second at 120 Hz, about 99.4% of the headset's pictures are new
frames. The GPU is 28 to 35% busy at 120 fps on the RTX 5090.

## 10. Measurement tools

These were added to find and check the work above. They write to the emulator's log, or to
files in its log folder (`user/log/`; for `pc-bench.sh` runs, `build/pc-bench/bin/user/log/`).

### Headset pictures

Code: `shadps4-arm64-main/src/core/vr/openxr_host.cpp`.

- **Every 10 s, a log line:** `Headset pictures: N of M repeated the frame before (x a second,
  at most k in a row), n frames were never shown (a newer one came first); frames waited a ms on
  average to be taken (b at worst)`.
- **`SHADPS4_XR_FRAME_LOG=1`** writes `xr_frames.csv`, one row per picture the headset took:
  - `picture`: the picture's number;
  - `ms`: when, since the XR session started;
  - `frame`: the id of the game's frame it showed;
  - `new`: 1 for a new frame, 0 for a repeat;
  - `waited_ms`: how long the frame waited between being handed over and being taken.
- **`tools/bench/xrframes.py`** sums that file up:
  `python tools/bench/xrframes.py <xr_frames.csv> [from_ms]`. `from_ms` defaults to 85000,
  inside level 1 of the scripted walk. It prints how many pictures repeated the one before
  (and how many a second), how many of the game's frames were never shown, the pictures around
  the first repeats, and percentiles of the wait.

### Frame stage times

`SHADPS4_FRAME_STATS_CSV=1` (in `renderer_vulkan/vk_scheduler.cpp`) works together with
`SHADPS4_FRAME_STATS=<level>`. It writes every frame's stage times to `frame_stats.csv` (`ms`,
`stage`, `took_ms`). The stages are submitting, catchup, translate, queued, gpuwait and
latency. Use it to find the frames that took longer.

### Guest mutex waits

`SHADPS4_MUTEX_STATS=<seconds>` (in `kernel/threads/mutex.cpp`). That often, the log lists the
eight guest mutexes that were waited on longest. Only waits where a lock had to block, after
its spinning, are counted. The lines look like `Mutex waits in 10 s: <address>
'<name>' waited on N times, X ms in all; by <thread> X ms, ...; held by <thread> X ms, ...`.
Each line gives the top four waiting threads and the top four threads that held the mutex
when the waits began. This is how the DrawThreadMuxtex waits in section 1 were found.

### Sampling profiler

`SHADPS4_PROFILE=<start s>,<seconds>[,<samples a second>]` (in `core/thread_profiler.cpp`,
Windows). It writes `profile_<thread>.txt`, and needs `shadps4.pdb` next to the executable.
New:

- **`SHADPS4_PROFILE_THREADS=<name>[,<name>...]`** profiles any threads by the names they give
  themselves, the game's own threads included (`Common::SetCurrentThreadName` starts the
  profile). Without it, only the GPU command thread is profiled, as before.
- The game's threads run on stacks the emulator made. For those, the stack walk takes its
  bounds from the committed memory around the stack pointer.
- **A new report section** lists the innermost return address in the game's code, as
  module+offset (as in a disassembly of the module loaded at 0), together with the emulator
  (HLE) function it was calling. Addresses outside every host module show as `[guest code]`.
- **`SHADPS4_PROFILE_RAW=1`** also writes every sample to `profile_<thread>.csv` (steady-clock
  ms, innermost function, guest place, callee), so the samples of chosen moments can be picked
  out.
- Threads with the same name get report names of their own (`_2`, `_3`, ...).

### Bench line

`SHADPS4_BENCH=<seconds>` now also reports:

- HR timers a frame, and how late they fired (average and maximum);
- how many stream copies the memo answered.

The stream-repeat analysis needs `SHADPS4_BENCH_STREAM=1`.

### pc-bench.sh

`tools/pc-bench.sh` has three new settings:

- `SCRIPT=<file>`: the input script of the runs;
- `GAME_ARGS="<args>"`: arguments passed to the game after `--`;
- `SAVE=<dir>`: the save the runs start from.

### OpenXR Simulator

`OPENXR_SIM_REFRESH_HZ=<30..500>` sets the refresh rate the simulator paces `xrWaitFrame` at
(default 90). The change is in the simulator, not in this repository.

## 11. Open findings

None of these are solved. They are where to look next.

1. **The remaining repeated pictures (about 0.75 a second at 120 Hz) come from the game's side.**
   About every 2.08 s (250 frames at 120 fps), one frame's submission grows from about 6 ms to
   11 to 12 ms. Submission here is the time from its first command list to `SubmitDone`. A few
   slower frames follow. This is being profiled (`SHADPS4_FRAME_STATS_CSV`,
   `SHADPS4_PROFILE_THREADS`, `SHADPS4_PROFILE_RAW`).
2. **One hitch at the same moment of the level-1 walk in every run.** For about 120 to 150 ms
   no new frame reaches the headset. In `xr_frames.csv` it is at about 93 s into the run.
3. **Where the GPU command thread's time goes at 120 fps** (40 s profile):
   - the thread is about 76% busy;
   - the NVIDIA driver is about 13.5% of the samples: push descriptors about 6.7%, draws about
     3.6%, vertex buffer binds about 2%;
   - the `memcpy` of the stream copies that are still made is about 6.7%;
   - the rest is spread over the texture and buffer caches and PM4 packet processing.
4. **Other levels can now be benchmarked.**
   [tools/re/astro-bot/save-format.md](../tools/re/astro-bot/save-format.md) decodes the save:
   - it is plain XML, with records `L1`..`L78`;
   - each record has `A` (unlocked 0/1), `B` (rescued bots bitmask), `C` (chameleon found),
     `D` (times cleared) and `E` (best time);
   - `L1` to `L25` are the levels of Worlds 1 to 5.

   [make_save.py](../tools/re/astro-bot/make_save.py) builds saves with Worlds 1 and 2, or all
   five worlds, cleared. The same notes give the directions of the planets in the world select,
   for writing input scripts. No other level has been measured yet.
5. **Starting a level directly does not work.** The game reads `/app0/args.txt`.
   - `-level` only replaces the first room request, which in the retail flow is the world
     select, not a level.
   - The debug `-room` path crashes in the emulator.
   - `-sequence demo` resets progress.

## 12. Reproducing the measurements

**Requirements:**

- Git Bash.
- A build at `build/win-x64/shadps4.exe` (or `EXE=...`), with its `.pdb` for profiles.
- Your own copy of the game in `games/CUSA12392/`.
- The OpenXR Simulator at `E:\Github\OpenXR-Simulator\bin\openxr_simulator.json`, or its
  `.json` given with `OPENXR_SIM=...`. Use a build with `OPENXR_SIM_REFRESH_HZ` for the 120 Hz
  runs.
- Nothing else running the emulator.

The first run plays the opening once (about 4 minutes) to make the save in World 1.

```sh
# Uncapped (emulated headset at 250 Hz): what the PC can do
tools/pc-bench.sh current 3
tools/pc-bench.sh current-off 3 SHADPS4_PERF_ALL=0        # every SHADPS4_PERF_* switch off
tools/pc-bench.sh no-timers 3 SHADPS4_PERF_PRECISE_TIMERS=0

# Paced at 120 Hz, game's 60 mode driven at 240 Hz
UNCAPPED=0 FPS_CAP=120 tools/pc-bench.sh play120 1 OPENXR_SIM_REFRESH_HZ=120

# Paced at 120 Hz, the game's own 120 mode, with the per-picture log
UNCAPPED=0 FPS_CAP=120 tools/pc-bench.sh native120 1 OPENXR_SIM_REFRESH_HZ=120 SHADPS4_TITLE_NATIVE_RATE=120 SHADPS4_XR_FRAME_LOG=1

# The same with the earlier present-thread settings, for comparison
UNCAPPED=0 FPS_CAP=120 tools/pc-bench.sh native120-old 1 OPENXR_SIM_REFRESH_HZ=120 SHADPS4_TITLE_NATIVE_RATE=120 SHADPS4_PRESENT_LOOKS=8 SHADPS4_VR_DELIVERY_PHASE=0.55
```

**Results:**

- Each run's log, `Bench:` lines and pictures go to `build/pc-bench/<name>/run-<n>/`.
- The table goes to `build/pc-bench/<name>/summary.txt`.
- `pc-bench.sh` does **not** copy `xr_frames.csv` (or `frame_stats.csv`, `profile_*`). After
  the run, copy them from `build/pc-bench/bin/user/log/`. The log folder is emptied when each
  run starts, so with several runs only the last one's files are there.

```sh
cp build/pc-bench/bin/user/log/xr_frames.csv build/pc-bench/native120/
python tools/bench/xrframes.py build/pc-bench/native120/xr_frames.csv
```

**Other settings of `pc-bench.sh`** (environment):

| Setting | Meaning |
| --- | --- |
| `EXE` | the build to measure (default `build/win-x64/shadps4.exe`) |
| `XR` | `sim` (default), `none` (monitor only), or any OpenXR runtime's `.json` |
| `UNCAPPED` | how often the emulated headset refreshes (250); `0` paces by the headset's display |
| `FPS_CAP` | with `UNCAPPED=0`, the most frames a second (90) |
| `RUN_SECONDS` | length of a run (170) |
| `FROM_WINDOW` | the first 10-second window counted (8) |
| `SCRIPT` | the input script (`tools/bench/level1-walk.txt`) |
| `GAME_ARGS` | arguments for the game, after `--` |
| `SAVE` | the save to start from (`build/pc-bench/save`) |

Arguments after the run count are `NAME=value` environment variables for the emulator (and for
the OpenXR runtime it loads, like `OPENXR_SIM_REFRESH_HZ`).

## Reference: environment variables

In the PC play folder, add them as `env=NAME=value` lines in `pc-vr\settings.txt`.

### Optimization switches (`SHADPS4_PERF_*`)

All are on by default. `SHADPS4_PERF_<NAME>=0` turns one off. `SHADPS4_PERF_ALL=0` turns all of
them off, and a switch named on its own wins over `ALL`.

| Switch | Default | Since | What it does |
| --- | --- | --- | --- |
| `IMAGE_FAST_STATE` | on | v0.13-perf1 | image bindings skip the texture cache lock when the image is settled |
| `FINDIMAGE_LOCKFREE` | on | v0.13-perf1 | repeated image lookups without the global texture cache lock |
| `SPEC_MEMO` | on | v0.13-perf1 | shader permutation memo per program |
| `LOG_LEVEL_FIRST` | on | v0.13-perf1 | log level checked before anything is formatted |
| `DUMP_CHECK` | on | v0.13-perf1 | per-draw register dump check is an atomic flag |
| `VIEW_MEMO` | on | v0.13-perf2 | each image remembers its last view |
| `PRECISE_TIMERS` | on | this work | HR timers of 1.2 ms or more on a precise timer thread |
| `PRECISE_SLEEP` | on | this work | one high-resolution waitable timer per thread for sleeps |
| `STREAM_MEMO` | on | this work | reuse of stream-buffer copies of the same data |

### Settings added in this work

| Variable | Default | Meaning |
| --- | --- | --- |
| `SHADPS4_TITLE_NATIVE_RATE` | unset (60) | the game's own frame rate option: `90`, or `1`/`120` for 120; set by the launcher for `fps=120` |
| `SHADPS4_TIMER_SPIN_US` | 300 | how long the precise timer thread spins before a deadline, 0 to 5000 µs |
| `SHADPS4_PRESENT_LOOKS` | 32 | the present thread's looks per refresh, 2 to 64 (2 when uncapped) |
| `SHADPS4_VR_DELIVERY_PHASE` | 0.35 | where in the host's refresh frames should arrive, 0.1 to 0.9 |
| `SHADPS4_XR_FRAME_LOG` | off | `1`: `xr_frames.csv`, one row per headset picture |
| `SHADPS4_FRAME_STATS_CSV` | off | `1` (with `SHADPS4_FRAME_STATS`): `frame_stats.csv`, every frame's stage times |
| `SHADPS4_MUTEX_STATS` | off | `<seconds>` (at least 1): log the guest mutexes waited on longest |
| `SHADPS4_PROFILE_THREADS` | GPU command thread | threads to profile by name, comma-separated (with `SHADPS4_PROFILE`) |
| `SHADPS4_PROFILE_RAW` | off | `1`: every profile sample in `profile_<thread>.csv` |
| `SHADPS4_BENCH_STREAM` | off | `1`: the `Bench:` line's stream-repeat analysis (costs GPU thread time) |
| `OPENXR_SIM_REFRESH_HZ` | 90 | OpenXR Simulator only: its refresh rate, 30 to 500 |

### Settings used above that existed before

| Variable | Meaning |
| --- | --- |
| `SHADPS4_BENCH=<seconds>` | the `Bench:` line every so many seconds |
| `SHADPS4_PROFILE=<start>,<seconds>[,<rate>]` | the sampling profiler |
| `SHADPS4_STACKS=<s>[,<s>...]` | every thread's stack at those times |
| `SHADPS4_FRAME_STATS=<level>` | per-frame stage statistics |
| `SHADPS4_VR_UNCAPPED=<Hz>` | uncapped benchmark mode, 60 to 1000 |
| `SHADPS4_VR_FPS_CAP=<n>` | the most frames a second (the launcher's `fps`) |
| `SHADPS4_TITLE_TIMESTEP=0` | the game's time step is left alone (the launcher's `real_time=0`) |
| `SHADPS4_EARLY_FLIP=0` | flip headset frames at refreshes only |

## Reference: where the code is

| Change | Files (under `shadps4-arm64-main/src/` unless noted) |
| --- | --- |
| Precise HR timers | `core/libraries/kernel/equeue.cpp`, `equeue.h` |
| Precise sleep | `common/thread.cpp` |
| Guest write epoch | `common/perf_toggles.h`, `video_core/amdgpu/liverpool.cpp`, `core/memory.cpp`, `video_core/buffer_cache/buffer_cache.cpp` |
| Stream memo | `video_core/buffer_cache/buffer_cache.cpp`, `buffer_cache.h`, `buffer.cpp`, `buffer.h` |
| Bench line additions | `core/bench_stats.cpp`, `bench_stats.h` |
| Native frame rate | `core/known_title.cpp`, `known_title.h`, `core/libraries/videoout/driver.cpp`, `pc-vr/launch.ps1`, `pc-vr/settings.txt` |
| Present looks, delivery phase | `core/libraries/videoout/driver.cpp` |
| Headset picture log | `core/vr/openxr_host.cpp`, `tools/bench/xrframes.py` |
| Frame stats CSV | `video_core/renderer_vulkan/vk_scheduler.cpp` |
| Mutex stats | `core/libraries/kernel/threads/mutex.cpp` |
| Profiler | `core/thread_profiler.cpp`, `thread_profiler.h`, `common/thread.cpp` |
| Benchmark script | `tools/pc-bench.sh` |
| Reverse-engineering notes | `tools/re/astro-bot/` |
