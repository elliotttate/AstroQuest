// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>
#include <cstring>

namespace Common {

/// Set by the GPU command thread for itself as it starts: what only that thread touches can go
/// without the locks other threads would need.
inline thread_local bool t_gpu_command_thread = false;

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
