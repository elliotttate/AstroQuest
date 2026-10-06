// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>

#include "common/types.h"

/// Measurements for comparing builds and settings on the PC. SHADPS4_BENCH=<seconds> (10 when
/// given as 1) writes a "Bench:" line to the log that often: how many frames the title handed
/// in and how long they took (average, 99th percentile, longest, how many took longer than 25,
/// 50 and 100 ms), how much processor time the GPU command thread and the whole emulator used
/// for a frame, how often an SSE4a instruction the processor lacks had to be emulated, and how
/// many shader modules and pipelines were compiled and how long that took. Without the setting
/// only the counters run, which cost an atomic add where something rare happens.
namespace Core::Bench {

/// Whether SHADPS4_BENCH is set.
bool Enabled();

/// Called by the GPU command thread itself once, as it starts.
void RegisterGpuThread();

/// Called when the title has handed in a frame (sceGnmSubmitDone).
void OnFrame();

/// An SSE4a instruction (EXTRQ, INSERTQ) the processor does not have was emulated once.
void CountEmulatedInstruction();

/// A shader permutation was looked up through the memo of GetProgram: found there or not.
void CountSpecMemo(bool hit);

/// A read-only guest buffer was copied into the stream buffer (by the GPU command thread),
/// while the command buffer with the given tick was being recorded. Counts how much, and how much
/// of it repeated a copy made before in the same command list of the title or the same command
/// buffer. Only with SHADPS4_BENCH; the repeats only with SHADPS4_BENCH_STREAM=1 as well (telling
/// them apart is expensive).
void CountStreamCopy(u64 address, u32 size, u64 tick);

/// A stream copy was not made: the same bytes, copied before, were used again.
void CountStreamMemoHit(u32 size);

/// The GPU command thread started on another command list of the title.
void OnCommandList();

/// An HR timer of the title fired this long after it was due.
void CountTimerLate(std::chrono::steady_clock::duration late);

/// Something was compiled on the GPU command thread: a shader module or a pipeline.
void CountShaderCompile(std::chrono::steady_clock::duration took);
void CountPipelineCompile(std::chrono::steady_clock::duration took);

/// Times what it lives through and counts it as a compile of either kind.
class CompileTimer {
public:
    explicit CompileTimer(bool pipeline_)
        : pipeline{pipeline_}, start{std::chrono::steady_clock::now()} {}
    ~CompileTimer() {
        const auto took = std::chrono::steady_clock::now() - start;
        pipeline ? CountPipelineCompile(took) : CountShaderCompile(took);
    }
    CompileTimer(const CompileTimer&) = delete;
    CompileTimer& operator=(const CompileTimer&) = delete;

private:
    bool pipeline;
    std::chrono::steady_clock::time_point start;
};

} // namespace Core::Bench
