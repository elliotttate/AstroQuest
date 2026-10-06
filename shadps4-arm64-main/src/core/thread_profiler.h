// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core::Profiler {

/// SHADPS4_PROFILE=<start seconds>,<seconds>[,<samples a second>]: a sampling profile of the
/// calling thread (the GPU command thread calls this as it starts). From <start> seconds on, for
/// <seconds>, the thread is stopped about 1000 times a second (or as often as given), its call
/// stack is taken, and it goes on. The emulator's own code only: the stack is walked with the
/// unwind data of its modules. The result, by function and by source line, goes to
/// <log folder>/profile_<thread>.txt; it needs the program's .pdb next to it to name functions.
/// Windows only; elsewhere this does nothing.
void ProfileCurrentThread(const char* thread_name);
/// (With SHADPS4_PROFILE_THREADS=<name>[,<name>...] the threads listed are profiled instead, by
/// the names they give themselves: Common::SetCurrentThreadName calls this.)

/// SHADPS4_STACKS=<seconds>[,<seconds>...]: at those times after this call, the call stack of
/// every thread of the emulator goes to <log folder>/stacks_<seconds>s.txt (each thread stopped
/// for a moment while its stack is taken). For finding out where things are stuck.
void DumpStacksLater();

} // namespace Core::Profiler
