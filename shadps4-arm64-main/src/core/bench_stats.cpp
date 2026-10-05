// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <vector>

#include "common/logging/log.h"
#include "core/bench_stats.h"

#ifdef _WIN32
#include <intrin.h>
#include <windows.h>
#endif

namespace Core::Bench {

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<u64> emulated_instructions{0};
std::atomic<u64> shader_compiles{0};
std::atomic<u64> shader_compile_ns{0};
std::atomic<u64> pipeline_compiles{0};
std::atomic<u64> pipeline_compile_ns{0};
std::atomic<void*> gpu_thread{nullptr};
std::atomic<u64> spec_memo_hits{0};
std::atomic<u64> spec_memo_misses{0};

double Period() {
    static const double period = [] {
        const char* value = std::getenv("SHADPS4_BENCH");
        if (value == nullptr) {
            return 0.0;
        }
        const double seconds = std::atof(value);
        return seconds <= 1.0 ? 10.0 : seconds;
    }();
    return period;
}

#ifdef _WIN32
u64 ThreadCycles(void* thread) {
    ULONG64 cycles = 0;
    if (thread == nullptr || !QueryThreadCycleTime(static_cast<HANDLE>(thread), &cycles)) {
        return 0;
    }
    return cycles;
}

u64 ProcessCycles() {
    ULONG64 cycles = 0;
    QueryProcessCycleTime(GetCurrentProcess(), &cycles);
    return cycles;
}

u64 Tsc() {
    return __rdtsc();
}
#else
u64 ThreadCycles(void*) {
    return 0;
}
u64 ProcessCycles() {
    return 0;
}
u64 Tsc() {
    return 0;
}
#endif

struct Window {
    Clock::time_point start;
    Clock::time_point last_frame;
    u64 tsc{};
    u64 gpu_cycles{};
    u64 process_cycles{};
    u64 emulated{};
    u64 shaders{};
    u64 shader_ns{};
    u64 pipelines{};
    u64 pipeline_ns{};
    u64 memo_hits{};
    u64 memo_misses{};
    std::vector<double> frames;

    void Begin(Clock::time_point now) {
        start = now;
        tsc = Tsc();
        gpu_cycles = ThreadCycles(gpu_thread.load(std::memory_order_relaxed));
        process_cycles = ProcessCycles();
        emulated = emulated_instructions.load(std::memory_order_relaxed);
        shaders = shader_compiles.load(std::memory_order_relaxed);
        shader_ns = shader_compile_ns.load(std::memory_order_relaxed);
        pipelines = pipeline_compiles.load(std::memory_order_relaxed);
        pipeline_ns = pipeline_compile_ns.load(std::memory_order_relaxed);
        memo_hits = spec_memo_hits.load(std::memory_order_relaxed);
        memo_misses = spec_memo_misses.load(std::memory_order_relaxed);
        frames.clear();
    }
};

} // namespace

bool Enabled() {
    return Period() > 0.0;
}

void RegisterGpuThread() {
#ifdef _WIN32
    HANDLE handle = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                        THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
        gpu_thread.store(handle, std::memory_order_relaxed);
    }
#endif
}

void CountEmulatedInstruction() {
    emulated_instructions.fetch_add(1, std::memory_order_relaxed);
}

void CountSpecMemo(bool hit) {
    (hit ? spec_memo_hits : spec_memo_misses).fetch_add(1, std::memory_order_relaxed);
}

void CountShaderCompile(Clock::duration took) {
    shader_compiles.fetch_add(1, std::memory_order_relaxed);
    shader_compile_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(took).count(),
        std::memory_order_relaxed);
}

void CountPipelineCompile(Clock::duration took) {
    pipeline_compiles.fetch_add(1, std::memory_order_relaxed);
    pipeline_compile_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(took).count(),
        std::memory_order_relaxed);
}

void OnFrame() {
    const double period = Period();
    if (period <= 0.0) {
        return;
    }
    static Window window;
    static bool started = false;
    const auto now = Clock::now();
    if (!started) {
        started = true;
        window.Begin(now);
        window.last_frame = now;
        return;
    }
    window.frames.push_back(std::chrono::duration<double, std::milli>(now - window.last_frame).count());
    window.last_frame = now;

    const double elapsed = std::chrono::duration<double>(now - window.start).count();
    if (elapsed < period) {
        return;
    }
    const u64 tsc = Tsc();
    const u64 gpu = ThreadCycles(gpu_thread.load(std::memory_order_relaxed));
    const u64 process = ProcessCycles();
    // The cycle counts are in units of the time stamp counter: its rate over the window turns
    // them into time.
    const double tsc_per_ms = tsc > window.tsc ? double(tsc - window.tsc) / (elapsed * 1000.0) : 0.0;
    const auto frames = window.frames.size();
    const auto per_frame_ms = [&](u64 now_cycles, u64 then_cycles) {
        if (tsc_per_ms <= 0.0 || frames == 0 || now_cycles < then_cycles) {
            return 0.0;
        }
        return double(now_cycles - then_cycles) / tsc_per_ms / double(frames);
    };
    std::vector<double> sorted = window.frames;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    u32 over25 = 0, over50 = 0, over100 = 0;
    for (const double ms : sorted) {
        sum += ms;
        over25 += ms > 25.0;
        over50 += ms > 50.0;
        over100 += ms > 100.0;
    }
    const double p99 = sorted.empty() ? 0.0 : sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)];
    const double longest = sorted.empty() ? 0.0 : sorted.back();
    const u64 emulated = emulated_instructions.load(std::memory_order_relaxed) - window.emulated;
    const u64 shaders = shader_compiles.load(std::memory_order_relaxed) - window.shaders;
    const u64 shader_ns = shader_compile_ns.load(std::memory_order_relaxed) - window.shader_ns;
    const u64 pipelines = pipeline_compiles.load(std::memory_order_relaxed) - window.pipelines;
    const u64 pipeline_ns = pipeline_compile_ns.load(std::memory_order_relaxed) - window.pipeline_ns;
    const u64 memo_hits = spec_memo_hits.load(std::memory_order_relaxed) - window.memo_hits;
    const u64 memo_misses = spec_memo_misses.load(std::memory_order_relaxed) - window.memo_misses;
    LOG_INFO(Core,
             "Bench: {} frames in {:.2f} s ({:.1f} a second); frame ms avg {:.2f} p99 {:.2f} max "
             "{:.1f}; over 25 ms {}, over 50 ms {}, over 100 ms {}; CPU ms a frame: GPU thread "
             "{:.3f}, process {:.3f}; SSE4a emulated {} ({:.1f} a frame); compiled {} shaders "
             "({:.1f} ms) and {} pipelines ({:.1f} ms); shader memo {:.1f}% of {} lookups",
             frames, elapsed, frames / elapsed, frames ? sum / frames : 0.0, p99, longest, over25,
             over50, over100, per_frame_ms(gpu, window.gpu_cycles),
             per_frame_ms(process, window.process_cycles), emulated,
             frames ? double(emulated) / frames : 0.0, shaders, shader_ns / 1e6, pipelines,
             pipeline_ns / 1e6,
             memo_hits + memo_misses ? 100.0 * memo_hits / double(memo_hits + memo_misses) : 0.0,
             memo_hits + memo_misses);
    window.Begin(now);
    window.last_frame = now;
}

} // namespace Core::Bench
