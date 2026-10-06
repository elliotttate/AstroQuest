// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/thread_profiler.h"

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>
// windows.h first
#include <dbghelp.h>
#include <tlhelp32.h>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"
#include "core/linker.h"

#pragma comment(lib, "dbghelp.lib")

namespace Core::Profiler {

namespace {

constexpr size_t MaxDepth = 48;

struct Sample {
    u32 depth{};
    std::array<u64, MaxDepth> pcs{};
    /// When it was taken: steady clock, milliseconds since its epoch.
    double ms{};
};


// Walks the stack of a stopped thread with the unwind data of the modules its frames are in.
// Nothing here may allocate: the stopped thread may hold the heap's lock. Nothing may fault
// either (the emulator's exception handler takes any fault it does not know for a crash): the
// walk stops at a stack pointer outside the thread's stack.
// Whether undoing this function's frame reads only the stack: RtlVirtualUnwind reads saved
// registers and the return address at offsets its unwind data gives, from the stack pointer or
// from a frame register, and a frame it got wrong would make it read anywhere. The frame's whole
// size and its farthest saved register are worked out from the unwind codes (following chained
// entries) and must lie within the stack; anything unusual ends the walk instead.
bool UnwindStaysOnStack(const CONTEXT& context, u64 image_base, const RUNTIME_FUNCTION* entry,
                        u64 stack_base, u64 stack_limit) {
    u64 frame_size = 8; // the return address
    u64 farthest_save = 0;
    u64 establisher = context.Rsp;
    bool first = true;
    for (int chain = 0; chain < 8; ++chain) {
        const auto* info = reinterpret_cast<const u8*>(image_base + entry->UnwindData);
        const u8 version = info[0] & 7;
        const u8 flags = info[0] >> 3;
        const u8 prolog_size = info[1];
        const u8 count = info[2];
        const u8 frame_register = info[3] & 15;
        const u8 frame_offset = info[3] >> 4;
        if (version != 1 && version != 2) {
            return false;
        }
        const auto* codes = info + 4;
        const u64 rip_offset = context.Rip - (image_base + entry->BeginAddress);
        if (first && frame_register != 0 && rip_offset >= prolog_size) {
            establisher = (&context.Rax)[frame_register] - u64{frame_offset} * 16;
        }
        for (u32 i = 0; i < count;) {
            const u8 op = codes[i * 2 + 1] & 15;
            const u8 op_info = codes[i * 2 + 1] >> 4;
            const auto slot = [&](u32 n) {
                return u64{codes[(i + n) * 2]} | (u64{codes[(i + n) * 2 + 1]} << 8);
            };
            switch (op) {
            case 0: // UWOP_PUSH_NONVOL
                frame_size += 8;
                i += 1;
                break;
            case 1: // UWOP_ALLOC_LARGE
                if (op_info == 0) {
                    frame_size += slot(1) * 8;
                    i += 2;
                } else {
                    frame_size += slot(1) | (slot(2) << 16);
                    i += 3;
                }
                break;
            case 2: // UWOP_ALLOC_SMALL
                frame_size += u64{op_info} * 8 + 8;
                i += 1;
                break;
            case 3: // UWOP_SET_FPREG
                i += 1;
                break;
            case 4: // UWOP_SAVE_NONVOL
                farthest_save = std::max(farthest_save, slot(1) * 8 + 8);
                i += 2;
                break;
            case 5: // UWOP_SAVE_NONVOL_FAR
                farthest_save = std::max(farthest_save, (slot(1) | (slot(2) << 16)) + 8);
                i += 3;
                break;
            case 6: // UWOP_EPILOG (version 2)
                i += 2;
                break;
            case 8: // UWOP_SAVE_XMM128
                farthest_save = std::max(farthest_save, slot(1) * 16 + 16);
                i += 2;
                break;
            case 9: // UWOP_SAVE_XMM128_FAR
                farthest_save = std::max(farthest_save, (slot(1) | (slot(2) << 16)) + 16);
                i += 3;
                break;
            default: // UWOP_PUSH_MACHFRAME and anything unknown
                return false;
            }
        }
        if ((flags & UNW_FLAG_CHAININFO) == 0) {
            break;
        }
        entry = reinterpret_cast<const RUNTIME_FUNCTION*>(codes + ((count + 1) & ~1u) * 2);
        first = false;
    }
    const u64 reach = std::max(frame_size, farthest_save);
    return (establisher & 7) == 0 && establisher >= stack_limit &&
           establisher + reach + 64 <= stack_base;
}

u32 WalkBounded(CONTEXT context, std::array<u64, MaxDepth>& pcs, u64 stack_base, u64 stack_limit) {
    // A title's thread runs on a stack the emulator made for it, not where its thread
    // information block says: the committed memory around its stack pointer is taken instead.
    if (context.Rsp < stack_limit || context.Rsp >= stack_base) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(context.Rsp), &info, sizeof(info)) ==
                sizeof(info) &&
            info.State == MEM_COMMIT) {
            stack_limit = reinterpret_cast<u64>(info.BaseAddress);
            stack_base = stack_limit + info.RegionSize;
        }
    }
    const auto on_stack = [&](u64 sp) {
        return (sp & 7) == 0 && sp >= stack_limit && sp + 256 <= stack_base;
    };
    u32 depth = 0;
    while (depth < MaxDepth && context.Rip != 0) {
        pcs[depth++] = context.Rip;
        if (!on_stack(context.Rsp)) {
            break;
        }
        DWORD64 image_base = 0;
        auto* entry = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (entry == nullptr) {
            // A leaf function, or code without unwind data (where the walk ends).
            if (depth > 1) {
                break;
            }
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        if (!UnwindStaysOnStack(context, image_base, entry, stack_base, stack_limit)) {
            break;
        }
        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, entry, &context,
                         &handler_data, &establisher, nullptr);
    }
    return depth;
}

u32 Walk(CONTEXT context, std::array<u64, MaxDepth>& pcs, const volatile u64* base_field,
         const volatile u64* limit_field) {
    return WalkBounded(context, pcs, *base_field, *limit_field);
}

struct Symbols {
    HANDLE process = GetCurrentProcess();
    std::unordered_map<u64, std::string> outer_names;
    std::unordered_map<u64, std::string> inner_names;
    std::unordered_map<u64, std::string> lines;

    Symbols() {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        SymInitialize(process, nullptr, TRUE);
    }
    ~Symbols() {
        SymCleanup(process);
    }

    std::string Module(u64 address) {
        IMAGEHLP_MODULE64 module{};
        module.SizeOfStruct = sizeof(module);
        if (SymGetModuleInfo64(process, address, &module)) {
            char text[160];
            std::snprintf(text, sizeof(text), "%s+%#llx", module.ModuleName,
                          static_cast<unsigned long long>(address - module.BaseOfImage));
            return text;
        }
        // Outside every module: the title's own code (or code made at run time).
        return "[guest code]";
    }

    // The function the address is in, as the compiler laid it out.
    const std::string& Outer(u64 address) {
        auto [it, is_new] = outer_names.try_emplace(address);
        if (is_new) {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 displacement = 0;
            it->second = SymFromAddr(process, address, &displacement, symbol)
                             ? std::string{symbol->Name}
                             : Module(address);
        }
        return it->second;
    }

    // The innermost function the address belongs to, inlined ones included.
    const std::string& Inner(u64 address) {
        auto [it, is_new] = inner_names.try_emplace(address);
        if (is_new) {
            it->second = Outer(address);
            if (SymAddrIncludeInlineTrace(process, address) > 0) {
                DWORD inline_context = 0;
                DWORD frame_index = 0;
                if (SymQueryInlineTrace(process, address, 0, address, address, &inline_context,
                                        &frame_index)) {
                    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
                    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
                    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                    symbol->MaxNameLen = 511;
                    DWORD64 displacement = 0;
                    if (SymFromInlineContext(process, address, inline_context, &displacement,
                                             symbol)) {
                        it->second = symbol->Name;
                    }
                }
            }
        }
        return it->second;
    }

    const std::string& Line(u64 address) {
        auto [it, is_new] = lines.try_emplace(address);
        if (is_new) {
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD displacement = 0;
            if (SymGetLineFromAddr64(process, address, &displacement, &line)) {
                std::string file = line.FileName;
                if (const auto slash = file.find_last_of("\\/"); slash != std::string::npos) {
                    file = file.substr(slash + 1);
                }
                it->second = file + ":" + std::to_string(line.LineNumber);
            } else {
                it->second = Outer(address);
            }
        }
        return it->second;
    }
};

bool IsWait(const std::string& name) {
    static constexpr std::array waits{"NtWait", "ZwWait", "NtDelayExecution", "WaitOnAddress",
                                      "RtlSleepConditionVariable", "NtAlertThreadByThreadId",
                                      "WaitForSingleObject", "WaitForMultipleObjects",
                                      "SleepConditionVariable", "SwitchToThread", "NtYield"};
    return std::ranges::any_of(waits, [&](const char* w) { return name.find(w) != std::string::npos; });
}

// What a sample was doing that is worth knowing the emulator's caller of: allocating, taking a
// lock, copying memory, or running in the graphics driver (whose functions have no symbols and
// show up as the nearest export of its module).
const char* SinkOf(const std::string& name) {
    static constexpr std::array<std::pair<const char*, const char*>, 15> sinks{{
        {"SpinLock::lock", "spin"},
        {"RtlAllocateHeap", "allocate"}, {"RtlFreeHeap", "free"}, {"malloc", "allocate"},
        {"operator new", "allocate"}, {"free_base", "free"}, {"RtlAcquireSRW", "lock"},
        {"Mtx_lock", "lock"}, {"RtlReleaseSRW", "lock"}, {"_NLG_Return", "memcpy"},
        {"memmove", "memcpy"}, {"memcpy", "memcpy"}, {"vkGetInstanceProcAddr", "driver"},
        {"vk_icd", "driver"}, {"vkEnumerateInstance", "driver"}}};
    for (const auto& [match, sink] : sinks) {
        if (name.find(match) != std::string::npos) {
            return sink;
        }
    }
    return nullptr;
}

bool IsEmulatorFunction(const std::string& name) {
    static constexpr std::array spaces{"Vulkan::", "VideoCore::", "AmdGpu::", "Shader::",
                                       "Core::", "Libraries::", "Common::", "Frontend::"};
    return std::ranges::any_of(spaces, [&](const char* s) { return name.starts_with(s); });
}

void Report(const std::vector<Sample>& samples, const std::string& thread_name, double seconds) {
    // (The symbol library is not to be used by two threads at once.)
    static std::mutex report_mutex;
    std::scoped_lock report_lock{report_mutex};
    Symbols symbols;
    std::unordered_map<std::string, u32> self, inclusive, by_line, sink_callers, guest_sites;
    std::unordered_map<u64, std::string> guest_names;
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    // A place in the title's code (or one of its modules): module+offset, as in a disassembly of
    // the module loaded at 0.
    const auto guest_name = [&](u64 address) -> const std::string& {
        auto [it, is_new] = guest_names.try_emplace(address);
        if (is_new) {
            char text[160];
            if (auto* module = linker->FindByAddress(address); module != nullptr) {
                std::snprintf(text, sizeof(text), "%s+%#llx", module->name.c_str(),
                              static_cast<unsigned long long>(address - module->GetBaseAddress()));
            } else {
                std::snprintf(text, sizeof(text), "%#llx", static_cast<unsigned long long>(address));
            }
            it->second = text;
        }
        return it->second;
    };
    u32 waiting = 0;
    for (const auto& sample : samples) {
        if (sample.depth == 0) {
            continue;
        }
        const u64 leaf = sample.pcs[0];
        const auto& leaf_name = symbols.Inner(leaf);
        // A thread that waits shows the wait at the top, and the caller that waits below it.
        bool waits = IsWait(symbols.Outer(leaf));
        for (u32 i = 1; i < std::min<u32>(sample.depth, 4) && !waits; ++i) {
            waits = IsWait(symbols.Outer(sample.pcs[i] - 1));
        }
        if (waits) {
            ++waiting;
        }
        ++self[leaf_name];
        ++by_line[symbols.Line(leaf)];
        // The first sink near the top of the stack, and the emulator function under it.
        const char* sink = nullptr;
        for (u32 i = 0; i < std::min<u32>(sample.depth, 8) && sink == nullptr; ++i) {
            const u64 pc = i == 0 ? leaf : sample.pcs[i] - 1;
            sink = SinkOf(i == 0 ? leaf_name : symbols.Outer(pc));
            if (sink == nullptr && i == 0) {
                sink = SinkOf(symbols.Outer(pc));
            }
            if (sink != nullptr) {
                for (u32 j = i + 1; j < sample.depth; ++j) {
                    const auto& caller = symbols.Outer(sample.pcs[j] - 1);
                    if (IsEmulatorFunction(caller)) {
                        ++sink_callers[std::string{sink} + " <- " + caller + " (" +
                                       symbols.Line(sample.pcs[j] - 1) + ")"];
                        break;
                    }
                }
            }
        }
        // The innermost frame in the title's code, and what of the emulator it was in.
        for (u32 i = 0; i < sample.depth; ++i) {
            if (symbols.Module(sample.pcs[i]) != "[guest code]") {
                continue;
            }
            const std::string callee =
                i == 0 ? std::string{"(running)"} : symbols.Outer(sample.pcs[i - 1] - (i > 1));
            ++guest_sites[guest_name(sample.pcs[i]) + "  " + callee];
            break;
        }
        std::unordered_set<std::string> seen;
        seen.insert(leaf_name);
        seen.insert(symbols.Outer(leaf));
        for (u32 i = 1; i < sample.depth; ++i) {
            seen.insert(symbols.Outer(sample.pcs[i] - 1));
        }
        for (const auto& name : seen) {
            ++inclusive[name];
        }
    }
    const auto sorted = [](const std::unordered_map<std::string, u32>& counts) {
        std::vector<std::pair<std::string, u32>> list(counts.begin(), counts.end());
        std::ranges::sort(list, [](const auto& a, const auto& b) { return a.second > b.second; });
        return list;
    };
    const auto total = static_cast<double>(samples.size());
    std::string text;
    char line[640];
    std::snprintf(line, sizeof(line),
                  "Profile of %s: %zu samples in %.1f s; %.1f%% of them waiting\n\n",
                  thread_name.c_str(), samples.size(), seconds, 100.0 * waiting / total);
    text += line;
    const auto section = [&](const char* title, const std::unordered_map<std::string, u32>& counts,
                             size_t rows) {
        text += title;
        text += "\n";
        size_t n = 0;
        for (const auto& [name, count] : sorted(counts)) {
            if (n++ == rows) {
                break;
            }
            std::snprintf(line, sizeof(line), "%6.2f%%  %s\n", 100.0 * count / total,
                          name.substr(0, 560).c_str());
            text += line;
        }
        text += "\n";
    };
    section("Self (the innermost function, inlined ones included):", self, 60);
    section("Inclusive (anywhere on the stack, functions as compiled):", inclusive, 80);
    section("Self by source line:", by_line, 60);
    section("Allocations, locks, copies and driver calls, by the emulator function that made them:",
            sink_callers, 80);
    section("The innermost place in the title's code (a return address, module+offset) and the "
            "emulator function it was in:",
            guest_sites, 60);

    // SHADPS4_PROFILE_RAW=1: every sample as well, in profile_<thread>.csv: when (steady clock,
    // ms), the innermost function, the innermost place in the title's code and what of the
    // emulator it was in, for picking out the samples of chosen moments.
    static const bool raw = std::getenv("SHADPS4_PROFILE_RAW") != nullptr;
    if (raw) {
        std::string csv = "ms,leaf,guest,callee\n";
        for (const auto& sample : samples) {
            if (sample.depth == 0) {
                continue;
            }
            std::string site = "-", callee = "-";
            for (u32 i = 0; i < sample.depth; ++i) {
                if (symbols.Module(sample.pcs[i]) != "[guest code]") {
                    continue;
                }
                site = guest_name(sample.pcs[i]);
                callee = i == 0 ? std::string{"(running)"}
                                : symbols.Outer(sample.pcs[i - 1] - (i > 1));
                break;
            }
            std::string leaf = symbols.Inner(sample.pcs[0]);
            for (std::string* text : {&leaf, &callee}) {
                std::ranges::replace(*text, ',', ';');
                if (text->size() > 120) {
                    text->resize(120);
                }
            }
            std::snprintf(line, sizeof(line), "%.3f,%s,%s,%s\n", sample.ms, leaf.c_str(),
                          site.c_str(), callee.c_str());
            csv += line;
        }
        const auto csv_path = Common::FS::GetUserPath(Common::FS::PathType::LogDir) /
                              ("profile_" + thread_name + ".csv");
        if (FILE* file = _wfopen(csv_path.c_str(), L"wb"); file != nullptr) {
            std::fwrite(csv.data(), 1, csv.size(), file);
            std::fclose(file);
        }
    }

    const auto path = Common::FS::GetUserPath(Common::FS::PathType::LogDir) /
                      ("profile_" + thread_name + ".txt");
    if (FILE* file = _wfopen(path.c_str(), L"wb"); file != nullptr) {
        std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
    }
    LOG_INFO(Core, "Profile of {} written to {} ({} samples)", thread_name, path.string(),
             samples.size());
}

void Run(HANDLE thread, std::string thread_name, double start, double seconds, double rate,
         const volatile u64* base_field, const volatile u64* limit_field) {
    {
        static std::mutex names_mutex;
        static std::unordered_map<std::string, u32> names;
        std::scoped_lock lock{names_mutex};
        if (const u32 seen = names[thread_name]++; seen != 0) {
            thread_name += "_" + std::to_string(seen + 1);
        }
    }
    std::this_thread::sleep_for(std::chrono::duration<double>(start));
    std::vector<Sample> samples;
    samples.reserve(static_cast<size_t>(seconds * rate * 1.2) + 64);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    const auto begin = std::chrono::steady_clock::now();
    const auto period = std::chrono::duration<double>(1.0 / rate);
    auto next = begin;
    while (samples.size() < samples.capacity()) {
        next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
        const auto now = std::chrono::steady_clock::now();
        if (now - begin >= std::chrono::duration<double>(seconds)) {
            break;
        }
        if (next > now) {
            LARGE_INTEGER due;
            due.QuadPart =
                -std::max<LONGLONG>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           next - now)
                                               .count() /
                                           100);
            if (timer != nullptr && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, INFINITE);
            } else {
                std::this_thread::sleep_until(next);
            }
        }
        if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
            break;
        }
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        Sample& sample = samples.emplace_back();
        sample.ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
        if (GetThreadContext(thread, &context)) {
            sample.depth = Walk(context, sample.pcs, base_field, limit_field);
        }
        ResumeThread(thread);
    }
    if (timer != nullptr) {
        CloseHandle(timer);
    }
    const double took =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    Report(samples, thread_name, took);
    CloseHandle(thread);
}

// Where a thread's stack is, from its thread information block (in this process, so readable).
bool StackBounds(HANDLE thread, u64& base, u64& limit) {
    struct ThreadBasicInformation {
        LONG exit_status;
        PVOID teb_base;
        PVOID client_id[2];
        ULONG_PTR affinity;
        LONG priority;
        LONG base_priority;
    };
    using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<NtQueryInformationThreadFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    ThreadBasicInformation info{};
    if (query == nullptr || query(thread, 0, &info, sizeof(info), nullptr) < 0 ||
        info.teb_base == nullptr) {
        return false;
    }
    const auto* tib = static_cast<const NT_TIB*>(info.teb_base);
    base = reinterpret_cast<u64>(tib->StackBase);
    limit = reinterpret_cast<u64>(tib->StackLimit);
    return base > limit;
}

struct ThreadStack {
    DWORD id{};
    u64 cpu_100ns{};
    Sample sample{};
};

void DumpAllStacks(double at_seconds) {
    std::vector<DWORD> ids;
    if (HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID == GetCurrentProcessId() &&
                entry.th32ThreadID != GetCurrentThreadId()) {
                ids.push_back(entry.th32ThreadID);
            }
        }
        CloseHandle(snapshot);
    }
    std::vector<ThreadStack> stacks(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        auto& stack = stacks[i];
        stack.id = ids[i];
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                       THREAD_QUERY_INFORMATION,
                                   FALSE, ids[i]);
        if (thread == nullptr) {
            continue;
        }
        FILETIME created, exited, kernel, user;
        if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
            stack.cpu_100ns = (u64{kernel.dwHighDateTime} << 32 | kernel.dwLowDateTime) +
                              (u64{user.dwHighDateTime} << 32 | user.dwLowDateTime);
        }
        u64 base = 0, limit = 0;
        const bool bounded = StackBounds(thread, base, limit);
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context)) {
                if (bounded) {
                    stack.sample.depth = WalkBounded(context, stack.sample.pcs, base, limit);
                } else {
                    stack.sample.pcs[0] = context.Rip;
                    stack.sample.depth = 1;
                }
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    Symbols symbols;
    std::string text;
    char line[700];
    for (const auto& stack : stacks) {
        std::string name;
        if (HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, stack.id)) {
            PWSTR description = nullptr;
            if (SUCCEEDED(GetThreadDescription(thread, &description)) && description != nullptr) {
                for (const wchar_t* c = description; *c != 0; ++c) {
                    name += *c < 128 ? static_cast<char>(*c) : '?';
                }
                LocalFree(description);
            }
            CloseHandle(thread);
        }
        std::snprintf(line, sizeof(line), "Thread %lu %s (CPU %.1f s)\n", stack.id, name.c_str(),
                      stack.cpu_100ns / 1e7);
        text += line;
        for (u32 i = 0; i < stack.sample.depth; ++i) {
            const u64 pc = i == 0 ? stack.sample.pcs[0] : stack.sample.pcs[i] - 1;
            std::snprintf(line, sizeof(line), "    %s  (%s)\n", symbols.Inner(pc).substr(0, 300).c_str(),
                          symbols.Line(pc).c_str());
            text += line;
        }
        text += "\n";
    }
    char file_name[64];
    std::snprintf(file_name, sizeof(file_name), "stacks_%.0fs.txt", at_seconds);
    const auto path = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / file_name;
    if (FILE* file = _wfopen(path.c_str(), L"wb"); file != nullptr) {
        std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
    }
    LOG_INFO(Core, "Stacks of {} threads written to {}", stacks.size(), path.string());
}

} // namespace

void DumpStacksLater() {
    const char* value = std::getenv("SHADPS4_STACKS");
    if (value == nullptr) {
        return;
    }
    std::vector<double> times;
    for (const char* at = value; *at != 0;) {
        char* end = nullptr;
        const double t = std::strtod(at, &end);
        if (end == at) {
            break;
        }
        times.push_back(t);
        at = *end == ',' ? end + 1 : end;
    }
    std::thread{[times] {
        const auto start = std::chrono::steady_clock::now();
        for (const double t : times) {
            std::this_thread::sleep_until(start + std::chrono::duration_cast<
                                                      std::chrono::steady_clock::duration>(
                                                      std::chrono::duration<double>(t)));
            DumpAllStacks(t);
        }
    }}.detach();
}

void ProfileCurrentThread(const char* thread_name) {
    const char* value = std::getenv("SHADPS4_PROFILE");
    if (value == nullptr) {
        return;
    }
    // SHADPS4_PROFILE_THREADS=<name>[,<name>...]: the threads to profile, by the names they give
    // themselves (each one starts its profile as it is named). Without it, the GPU command
    // thread only.
    if (const char* threads = std::getenv("SHADPS4_PROFILE_THREADS"); threads != nullptr) {
        const std::string_view list{threads};
        const std::string_view name{thread_name};
        bool listed = false;
        for (size_t at = 0; at <= list.size();) {
            const size_t comma = std::min(list.find(',', at), list.size());
            listed |= list.substr(at, comma - at) == name;
            at = comma + 1;
        }
        if (!listed) {
            return;
        }
    } else if (std::string_view{thread_name} != "GpuCommandProcessor") {
        return;
    }
    double start = 30.0, seconds = 30.0, rate = 1000.0;
    std::sscanf(value, "%lf,%lf,%lf", &start, &seconds, &rate);
    rate = std::clamp(rate, 10.0, 10000.0);
    // (Called on the thread to profile: its own thread information block.)
    const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const auto* base_field = reinterpret_cast<const volatile u64*>(&tib->StackBase);
    const auto* limit_field = reinterpret_cast<const volatile u64*>(&tib->StackLimit);
    HANDLE thread = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                         FALSE, 0)) {
        return;
    }
    std::string name = thread_name;
    std::ranges::replace_if(name, [](char c) { return c == ':' || c == ' '; }, '_');
    LOG_INFO(Core, "Profiling {} from {} s on for {} s, {} samples a second", thread_name, start,
             seconds, rate);
    std::thread{Run, thread, std::move(name), start, seconds, rate, base_field, limit_field}.detach();
}

} // namespace Core::Profiler

#else

namespace Core::Profiler {
void ProfileCurrentThread(const char*) {}
void DumpStacksLater() {}
} // namespace Core::Profiler

#endif
