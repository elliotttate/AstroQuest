// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "common/types.h"

namespace Common {

/// Set by the GPU command thread for itself as it starts: what only that thread touches can go
/// without the locks other threads would need.
inline thread_local bool t_gpu_command_thread = false;

/// Counts the emulator's own writes to guest memory on the GPU's behalf (fences and labels,
/// packets that write or copy memory, fills) and the moments the GPU command thread stops in a
/// command list (when the title or another queue may write): what was read from guest memory
/// before the count moved may no longer be what is there.
inline std::atomic<u64> g_guest_write_epoch{0};
inline void NoteGuestWrite() {
    g_guest_write_epoch.fetch_add(1, std::memory_order_relaxed);
}
inline u64 GuestWriteEpoch() {
    return g_guest_write_epoch.load(std::memory_order_relaxed);
}

/// Switches for the optimizations of the GPU command thread, so that each can be measured
/// against the code it replaces in one build: SHADPS4_PERF_<NAME>=0 turns one off,
/// SHADPS4_PERF_ALL=0 turns all of them off (a switch named on its own wins over ALL).
/// Read once, where they are used, into a static.
inline bool PerfToggle(const char* name, bool default_on = true) {
    char key[96] = "SHADPS4_PERF_";
    std::strncat(key, name, sizeof(key) - std::strlen(key) - 1);
    if (const char* value = std::getenv(key); value != nullptr) {
        return std::atoi(value) != 0;
    }
    if (const char* all = std::getenv("SHADPS4_PERF_ALL"); all != nullptr) {
        return std::atoi(all) != 0;
    }
    return default_on;
}

} // namespace Common
