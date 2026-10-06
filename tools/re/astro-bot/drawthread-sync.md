# ASTRO BOT (CUSA12392): DrawThread / Game:Main / Hmd::ReproThread frame sync

Static analysis of eboot.elf (addresses = eboot base 0). Nothing was run.

## Objects

| What | Address | Notes |
|---|---|---|
| Display manager | `*(0x2e3f040)` | `this` (r12/r15) in the functions below |
| DrawThreadMuxtex | dm+0xa20 | created 0xbe5e4f (`0xe382c0`), name 0x1269c36 |
| DrawThreadCond | dm+0xa48 | created 0xbe5e6c (`0xe37330`), bound to dm+0xa20, name 0x1269c47 |
| Hand-off state | dm+0xa58 | 1 = frame handed to DrawThread, 2 = DrawThread done |
| Frame index: Main / DrawThread | dm+0x948 / dm+0x94c | |
| Output mode | dm+0xa80 | 0xc = headset (HMD path 0xbe8f70), 0x22 = TV (0xbe8c10) |
| FrameRate option / HMD refresh | dm+0x9a0 / dm+0xa8c | 60 -> 120 Hz refresh, 90 -> 90, 120 -> 120 (0xbe6aa3..0xbe6ad4) |
| Draw workers | dm+0xa60 "Draw Shadow", +0xa68 "Draw Hmd", +0xa70 "Draw SocialScreen", +0xa78 "Draw SS Shadow" | each has a mutex at +0x70, a cond at +0x98 and a state at +0xa8 (1 = go, -1 = done); loop at 0xbe8690 |
| Hmd::ReproMutex | 0x2e42f90 | |
| Hmd::HmdWaitDrawCond | 0x2e42fb8 | string 0x126eb42. The only signal comes from the HR-timer handler (0xc4057f) |
| "Reprojection EOP QUEUE" | `*(0x2e43130)` | holds the GNM EOP event id 0x40, user event 3 (= sceHmdReprojectionSetUserEventEnd) and the VideoOut vblank event |
| tick count | 0x2e460cc | +1 for each 3 ms timer that fires after a vblank |
| tick target | 0x2e460c8 | |
| last frame handed to the reprojection | 0x2e460c0 | |
| reprojection "end" count | 0x2e460d0 | +1 for each user event 3, which the emulator sends at every vblank |
| display slot per frame | 0x2e460e0[8] | |
| PresentRate | 0x2e460c4 | |
| enable | 0x2e460d4 | |
| Repro EOP labels | `*(0x2e430e0)`[4] | the GPU writes the frame index here |
| 3 ms timespec | 0x165c5f8 = {0, 3000000} (tv_nsec at **0x165c600**) | in the RW data segment; only one reference, `lea rdx` at 0xc40531 |

## 1. DrawThread loop

The thread is created at 0xbe5e9b (`0xe38870`, entry 0xbe8a30, arg = dm), and its entry loops forever on `0xbe8ad0`:

```
be8b04  call e384b0            ; LOCK DrawThreadMuxtex (dm+0xa20)
be8b09  cmp [dm+0xa58],1 / be8b36 call e374e0 (DrawThreadCond)   ; wait for Main's hand-off (releases the mutex)
be8b91  call be8c10  (mode&2, TV)   |  be8ba9 call be8f70 (mode&4, HMD)   ; whole frame, MUTEX HELD
be8bbc  xchg [dm+0xa58],2 ; be8bce signal DrawThreadCond ; be8bda call e386b0  ; UNLOCK
```

HMD frame `0xbe8f70`. Everything below runs while DrawThreadMuxtex is held:

```
be9007..be9121  kick workers (lock w+0x70, ++w+0xac, w+0xa8=1, signal w+0x98)
be9126  call c6abc0        ; begin frame context, ring of 3 -> c64a40 -> c7d740:
                           ;   sceKernelWaitEqueue("CCommandContext EOP QUEUE", 1 us polls) until the
                           ;   GPU label of frame N-3 is written           <- GPU-completion wait
be9132/913f call be9990 x2 ; eyes
be9157  call c408c0        ; (mode 0xc) EVENT_WRITE_EOP + interrupt: label[N&3] = N  (eaddb0, pkt 0xc0044700)
be918e  call c6ac30        ; end frame context (c7d610: EOP label for the ring)
be92e5..be9458  for each worker: lock w+0x70; while [w+0xa8]!=-1 cond_wait(w+0x98)   <- worker waits
be9548  call c40d40(frame) ; HMD FRAME START: tick wait            <- the long wait
        (other modes: sceVideoOutGetVblankStatus / sceVideoOutWaitVblank at be9570/be9585)
be95d2..be964d  submit the workers' command buffers (vtbl+0x20 = f89710 -> sceGnmSubmitCommandBuffers @f89a75)
be9663  c6aca0 ; be975c f8fa90
```

Frame start `0xc40d40` (waits on HmdWaitDrawCond with **Hmd::ReproMutex**):

```
c40d6b  lock ReproMutex
c40d70  while (frame - [2e460c0] > 2  ||  [2e460cc] < [2e460c8])
c40da6      cond_wait(HmdWaitDrawCond 2e42fb8, ReproMutex)
c40dca  n = (PresentRate==60) ? 2 : 1
c40df1  [2e460c8] = tick_now + n          ; next frame's target
c40e0b  slot[frame&7] = [2e460d0] + n      ; reprojection may show this frame after n more "end" events
```

**Answer:** DrawThread holds DrawThreadMuxtex for the whole frame. While holding it, it does three kinds of waiting:

* It cond_waits on **Hmd::HmdWaitDrawCond / Hmd::ReproMutex** at 0xc40da6.
* It cond_waits on the worker conds (w+0x98 / w+0x70) at 0xbe9306, 0xbe9366, 0xbe93d6 and 0xbe9436.
* It polls the GPU label at 0xc7d79e and 0xc7d832.

Its only cond_wait that does *not* hold the mutex is the idle wait for Main's hand-off at 0xbe8b36.

The workers use little CPU. In bench/results/hunt-3/stacks_70s.txt over 70 s, Draw Hmd used 1.9 s and Draw Shadow 1.0 s. So the 78.5% is almost all the **tick wait in 0xc40d40**. That wait has two conditions:

* (a) **tick ≥ target**, where target = the tick count at the previous frame start + 2 at FrameRate 60, or +1 at 90/120.
* (b) **frame N-2 has been handed to the reprojection**, i.e. `[2e460c0] ≥ N-2`.

The signal comes from **Hmd::ReproThread** (0xc40420), *only* in its HR-timer branch:

```
c40472  sceKernelWaitEqueue(Reprojection EOP QUEUE, 1 event)
c4049f  filter == -13 (VideoOut vblank): GetFlipStatus; c40538 sceKernelAddHRTimerEvent(eq, id 4, &{0,3ms})
c4055c  id 4  (timer):  c4056f inc [2e460cc]; c4057f signal HmdWaitDrawCond; c40590 DeleteHRTimerEvent(4)
c40550  id 3  (repro end, user ev): c405a3 inc [2e460d0]; c405a9 call c40980; delete/re-add user ev 3
c40556  id 0x40 (GPU EOP):  c405e2 call c40980
```

`0xc40980` hands frames to the reprojection. Let cur = [2e460c0]. The function goes on only if all of these hold:

* the GPU has written label[(cur+1)&3] = cur+1 (EOP),
* `[2e460d0] ≥ slot[(cur+1)&7]`.

It then calls sceHmdReprojectionStart (0xc40c94) and sets `[2e460c0] = cur+1` (0xc40ca1).

Neither the EOP path nor the user-event-3 path signals the cond. If condition (b) is what blocks, it is only checked again at the **next tick**.

## 2. Game:Main and DrawThreadMuxtex

Main's display step is `0xbe61d0`, called from 0xbe6120, which is called from the main loop at 0x4b20.

```
be61f2  call be66b0 (headset output state; sets PresentRate via c406c0 / mode dm+0xa80)
be620b  ++[dm+0x948]               ; new frame index
be6333  call [vtbl+0x100]          ; build the frame's render data (timed by dm+0xa90)
be646d  start "sync" timer dm+0xb20
be6481  call e384b0  -> e384cf      ; LOCK DrawThreadMuxtex   <== 63% of Main is spent blocked here
be64a6  while [dm+0xa58]!=2 cond_wait(DrawThreadCond)   (normally already 2)
be64da  [dm+0x94c] = [dm+0x948]     ; give DrawThread the new frame
be6502..be656e  flags 0x9a4/0x9a5, f07bd0/f0ada0 (swap the frame's draw data)
be6661  xchg [dm+0xa58],1 ; be6669 signal ; be6675 unlock
```

Main hands frame N+1 to DrawThread. It can do that only once DrawThread has finished frame N, and "finished" includes frame N's tick wait and submit. Main never waits for the GPU directly. It is blocked because DrawThread holds the mutex across 0xc40d40. The other users of this mutex, 0xbe7e70 and 0xbe7f00, are virtual "wait for state" and "set state + unlock" helpers that the main loop does not call directly.

## 3. Pipeline per frame, and the one wait chain

```
emulated vblank --(VideoOut ev)--> ReproThread: AddHRTimerEvent(3 ms)
   --3 ms--> timer id 4 --> tick++ , signal HmdWaitDrawCond
   --> DrawThread leaves c40d40 (needs tick>=target AND lastRepro>=N-2), target = tick+2 (60) / +1
   --> submit frame N (f89a75) --> state=2, unlock DrawThreadMuxtex
   --> Main (blocked at be6481) hands over N+1 --> DrawThread: frame-context wait (GPU N-3),
       workers, EOP label write, ... --> c40d40 again
GPU: EOP of N (label + interrupt id 0x40) --> ReproThread c40980 --> sceHmdReprojectionStart(N)
       once 2 (60) / 1 (90/120) "end" events have passed since N started (user ev 3 = every vblank)
```

* What starts a frame is the **3 ms post-vblank timer tick**: **2 ticks per frame at FrameRate 60**, **1 tick at 90/120**.
* It is not the vblank itself, not a flip, and not GPU completion. The vblank wait is used only outside HMD mode.
* GPU completion enters the chain in two places:
  * the N-2 condition of 0xc40d40, which is checked only on ticks;
  * the ring wait for frame N-3 when DrawThread begins its frame (0xc7d740).
* With Main busy only ~37% of the time (~3.3 ms) and DrawThread's own work small, the period is **max(2 ticks, Main + hand-off, DrawThread's work before the wait)**, which is 2 ticks.
* A frame that arrives late but before the tick after its target loses nothing extra, because the next target is counted in ticks.
* A frame loses a whole tick when either of these happens:
  * a tick is never produced;
  * condition (b) fails, which happens when frame N-2's EOP or display slot comes after frame N's tick.

  Such a frame takes 3 ticks: 12.5 ms at a 240 Hz tick.
* 112 fps works out to about 16 extra ticks per second: 240 ticks/s means 120 fps, and each lost tick costs about 1 fps. That is roughly 7% of vblanks producing no usable tick.

## 4. How the emulator could shorten the chain

1. **Do not lose ticks at 240 Hz (likely the main gap).**
   * At 240 Hz a 3 ms timer leaves only 1.17 ms before the next vblank. On hardware it leaves 5.3 ms at 120 Hz.
   * If vblank k+1 reaches the ReproThread before timer k has fired, `AddHRTimerEvent(id 4)` re-arms the pending timer. Under kqueue semantics, and in equeue.cpp `AddEvent` (where a duplicate only updates the interval), that leaves one fire for two vblanks, so one tick is lost.
   * The same happens if two vblank events merge before the ReproThread reads them (VideoOut events carry the Clear flag).
   * Causes include the present thread's looks or AccurateTimer catch-up bunching vblanks, an inline Flip, timer lateness, or wake latency.
   * Fixes:
     * **Scale the constant.** Write tv_nsec at base+**0x165c600** = 3,000,000 × 120 / vblank_hz, i.e. 1,500,000 at 240 Hz. This is safe:
       * only 0xc40531 uses it;
       * the tick and vblank counts stay the same, so game speed does not change;
       * pose prediction (0xc40e30) uses frame count × refresh period, not this offset;
       * the only effect is that frames start earlier after a synthetic vblank, and HeadsetRefreshClock's delay feedback re-adapts.
     * **Or don't merge re-arms** for this title. In sceKernelAddHRTimerEvent, when id 4 on this queue is still pending, trigger it now instead of merging.
     * **Or fire it at the vblank** (0 ms). This path goes through AddSmallTimer.
     * Also keep the gap between consecutive vblanks ≥ ~3.5 ms.
2. **Native 120 mode** (FrameRate 120, 1 tick per frame at a 120 Hz vblank) has the hardware's 5.3 ms margin, so lost ticks are much rarer. Each one that does happen costs a full 8.3 ms frame.
   * The engine step then really is 1/120 (EngineFrameRate set from dm+0x9a0 at 0xbe6b2c).
   * The game's pose prediction (0xc40e30: (N − lastFlipArg) << (FrameRate==60) × 1001000/refresh) is correct only in native 120. In 60@240 it assumes 2 × 8.34 ms per frame, i.e. twice too far ahead, which matters only if the predicted time is used.
3. **Deliver the repro GPU EOP (id 0x40 on "Reprojection EOP QUEUE") promptly.**
   * Frame N-2 must be EOP'd, and its slot reached (2 or 1 vblanks after its start), before frame N's tick.
   * Otherwise DrawThread sleeps a further full tick, because only the timer signals the cond.
   * Likewise, the "CCommandContext EOP QUEUE" label for N-3 delays DrawThread's start (0xc7d740).
4. Releasing DrawThreadMuxtex before 0xc40d40 (a code patch) would not raise the limit, which is set by ticks per frame. It is not needed while Main + DrawThread fit in one frame.
5. **Diagnostics before changing anything:**
   * Have the profiler also record the caller of the thunk, `[rbp+8]` of the thunk frame:
     * 0xc40dab = tick wait;
     * 0xbe930b, 0xbe936b, 0xbe93db, 0xbe943b = worker waits;
     * 0xbe8b3b = idle, waiting for Main;
     * 0xc7d7a3 and 0xc7d837 = GPU ring wait (sceKernelWaitEqueue).
   * At each id-4 timer on the repro queue, log:
     * vblanks delivered;
     * timers fired;
     * re-arms that found a pending id 4 (these are the lost ticks);
     * the guest values [0x2e460cc], [0x2e460c8], [0x2e460c0] and DisplayManager+0x94c (pointer at 0x2e3f040). This shows whether (a) or (b) blocked.

**Risks:**
* Changing how many vblanks or ticks occur per second changes what the title measures (dm+0xa8c-based stats in 0xbe6c30) and the prediction horizon. Scaling only the 3 ms offset changes neither.
* The no-merge HR-timer change differs from kqueue semantics, so limit it to CUSA12392 and this queue.
