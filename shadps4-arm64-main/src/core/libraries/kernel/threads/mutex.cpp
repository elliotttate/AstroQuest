// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <unordered_map>
#include <vector>
#include "common/arch.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/types.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/libs.h"

namespace Libraries::Kernel {

static constexpr u32 MUTEX_ADAPTIVE_SPINS = 2000;
static std::mutex MutxStaticLock;

// SHADPS4_MUTEX_STATS=<seconds>: for finding what the title's threads wait for. That often, the
// log gets the guest mutexes waited on longest (where a lock had to sleep): how long, by which
// threads, and which threads held them when the waits began.
namespace {
struct MutexWaits {
    std::string name;
    u64 waits{};
    u64 ns{};
    std::unordered_map<std::string, u64> waiters;
    std::unordered_map<std::string, u64> owners;
};

double MutexStatsPeriod() {
    static const double period = [] {
        const char* value = std::getenv("SHADPS4_MUTEX_STATS");
        return value != nullptr ? std::max(1.0, std::atof(value)) : 0.0;
    }();
    return period;
}

std::string ThreadName(const Pthread* thread) {
    return thread != nullptr ? thread->name : std::string{"(none)"};
}

void NoteMutexWait(const PthreadMutex* mutex, const std::string& owner, const std::string& waiter,
                   u64 ns) {
    using Clock = std::chrono::steady_clock;
    static std::mutex stats_lock;
    static std::unordered_map<const PthreadMutex*, MutexWaits> stats;
    static Clock::time_point window = Clock::now();
    std::scoped_lock lock{stats_lock};
    auto& entry = stats[mutex];
    if (entry.waits == 0) {
        entry.name = mutex->name;
    }
    ++entry.waits;
    entry.ns += ns;
    entry.waiters[waiter] += ns;
    entry.owners[owner] += ns;
    const auto now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - window).count();
    if (seconds < MutexStatsPeriod()) {
        return;
    }
    std::vector<std::pair<const PthreadMutex*, MutexWaits*>> sorted;
    for (auto& [address, waits] : stats) {
        sorted.emplace_back(address, &waits);
    }
    std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second->ns > b.second->ns; });
    const auto top = [](const std::unordered_map<std::string, u64>& by) {
        std::vector<std::pair<std::string, u64>> list{by.begin(), by.end()};
        std::ranges::sort(list, [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string text;
        for (size_t i = 0; i < std::min<size_t>(list.size(), 4); ++i) {
            text += fmt::format("{}{} {:.0f} ms", i ? ", " : "", list[i].first, list[i].second / 1e6);
        }
        return text;
    };
    for (size_t i = 0; i < std::min<size_t>(sorted.size(), 8); ++i) {
        const auto& [address, waits] = sorted[i];
        LOG_INFO(Kernel_Pthread,
                 "Mutex waits in {:.0f} s: {} '{}' waited on {} times, {:.1f} ms in all; by {}; "
                 "held by {}",
                 seconds, fmt::ptr(address), waits->name, waits->waits, waits->ns / 1e6,
                 top(waits->waiters), top(waits->owners));
    }
    stats.clear();
    window = now;
}
} // Anonymous namespace

#define THR_MUTEX_INITIALIZER ((PthreadMutex*)NULL)
#define THR_ADAPTIVE_MUTEX_INITIALIZER ((PthreadMutex*)1)
#define THR_MUTEX_DESTROYED ((PthreadMutex*)2)

#if defined(ARCH_X86_64)
#define CPU_SPINWAIT __asm__ volatile("pause")
#elif defined(ARCH_ARM64)
#define CPU_SPINWAIT __asm__ volatile("yield")
#endif

#define CHECK_AND_INIT_MUTEX                                                                       \
    if (PthreadMutex* m = *mutex; m <= THR_MUTEX_DESTROYED) [[unlikely]] {                         \
        if (s32 ret = InitStatic(g_curthread, mutex); ret) {                                       \
            return ret;                                                                            \
        }                                                                                          \
        m = *mutex;                                                                                \
    }

static constexpr PthreadMutexAttr PthreadMutexattrDefault = {
    .m_type = PthreadMutexType::ErrorCheck, .m_protocol = PthreadMutexProt::None, .m_ceiling = 0};

static constexpr PthreadMutexAttr PthreadMutexattrAdaptiveDefault = {
    .m_type = PthreadMutexType::AdaptiveNp, .m_protocol = PthreadMutexProt::None, .m_ceiling = 0};

using CallocFun = void* (*)(size_t, size_t);

static s32 MutexInit(PthreadMutexT* mutex, const PthreadMutexAttr* mutex_attr, const char* name) {
    const PthreadMutexAttr* attr;
    if (mutex_attr == nullptr) {
        attr = &PthreadMutexattrDefault;
    } else {
        attr = mutex_attr;
        if (attr->m_type < PthreadMutexType::ErrorCheck || attr->m_type >= PthreadMutexType::Max) {
            return POSIX_EINVAL;
        }
        if (attr->m_protocol > PthreadMutexProt::Protect) {
            return POSIX_EINVAL;
        }
    }
    auto* pmutex = new (std::nothrow) PthreadMutex{};
    if (pmutex == nullptr) {
        return POSIX_ENOMEM;
    }

    if (name) {
        pmutex->name = name;
    } else {
        static std::atomic<s32> MutexId{0};
        pmutex->name = fmt::format("Mutex{}", MutexId.fetch_add(1));
    }

    pmutex->m_flags = PthreadMutexFlags(attr->m_type);
    pmutex->m_owner = nullptr;
    pmutex->m_count = 0;
    pmutex->m_spinloops = 0;
    pmutex->m_yieldloops = 0;
    pmutex->m_protocol = attr->m_protocol;
    if (attr->m_type == PthreadMutexType::AdaptiveNp) {
        pmutex->m_spinloops = MUTEX_ADAPTIVE_SPINS;
        // pmutex->m_yieldloops = _thr_yieldloops;
    }

    *mutex = pmutex;
    return 0;
}

static s32 InitStatic(Pthread* thread, PthreadMutexT* mutex) {
    std::scoped_lock lk{MutxStaticLock};

    if (*mutex == THR_MUTEX_INITIALIZER || *mutex == THR_MUTEX_DESTROYED) {
        return MutexInit(mutex, &PthreadMutexattrDefault, nullptr);
    } else if (*mutex == THR_ADAPTIVE_MUTEX_INITIALIZER) {
        return MutexInit(mutex, &PthreadMutexattrAdaptiveDefault, nullptr);
    }
    return 0;
}

s32 PS4_SYSV_ABI posix_pthread_mutex_init(PthreadMutexT* mutex,
                                          const PthreadMutexAttrT* mutex_attr) {
    return MutexInit(mutex, mutex_attr ? *mutex_attr : nullptr, nullptr);
}

s32 PS4_SYSV_ABI scePthreadMutexInit(PthreadMutexT* mutex, const PthreadMutexAttrT* mutex_attr,
                                     const char* name) {
    return MutexInit(mutex, mutex_attr ? *mutex_attr : nullptr, name);
}

s32 PS4_SYSV_ABI posix_pthread_mutex_destroy(PthreadMutexT* mutex) {
    PthreadMutexT m = *mutex;
    if (m < THR_MUTEX_DESTROYED) {
        return 0;
    }
    if (m == THR_MUTEX_DESTROYED) {
        return POSIX_EINVAL;
    }
    if (m->m_owner != nullptr) {
        return POSIX_EBUSY;
    }
    *mutex = THR_MUTEX_DESTROYED;
    delete m;
    return 0;
}

s32 PthreadMutex::SelfTryLock() {
    switch (Type()) {
    case PthreadMutexType::ErrorCheck:
    case PthreadMutexType::Normal:
    case PthreadMutexType::AdaptiveNp:
        return POSIX_EBUSY;
    case PthreadMutexType::Recursive: {
        /* Increment the lock count: */
        if (m_count + 1 > 0) {
            m_count++;
            return 0;
        }
        return POSIX_EAGAIN;
    }
    default:
        return POSIX_EINVAL;
    }
}

s32 PthreadMutex::SelfLock(const OrbisKernelTimespec* abstime, u64 usec) {
    const auto DoSleep = [&] {
        if (abstime == THR_RELTIME) {
            std::this_thread::sleep_for(std::chrono::microseconds(usec));
            return POSIX_ETIMEDOUT;
        } else {
            if (abstime->tv_sec < 0 || abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000) {
                return POSIX_EINVAL;
            } else {
                std::this_thread::sleep_until(abstime->TimePoint());
                return POSIX_ETIMEDOUT;
            }
        }
    };
    switch (Type()) {
    case PthreadMutexType::ErrorCheck:
    case PthreadMutexType::AdaptiveNp: {
        if (abstime) {
            return DoSleep();
        }
        /*
         * POSIX specifies that mutexes should return
         * EDEADLK if a recursive lock is detected.
         */
        return POSIX_EDEADLK;
    }
    case PthreadMutexType::Normal: {
        /*
         * What SS2 define as a 'normal' mutex.  Intentionally
         * deadlock on attempts to get a lock you already own.
         */
        if (abstime) {
            return DoSleep();
        }
        UNREACHABLE_MSG("Mutex deadlock occured");
        return 0;
    }
    case PthreadMutexType::Recursive: {
        /* Increment the lock count: */
        if (m_count + 1 > 0) {
            m_count++;
            return 0;
        }
        return POSIX_EAGAIN;
    }
    default:
        return POSIX_EINVAL;
    }
}

s32 PthreadMutex::Lock(const OrbisKernelTimespec* abstime, u64 usec) {
    Pthread* curthread = g_curthread;
    if (m_owner == curthread) {
        return SelfLock(abstime, usec);
    }

    /*
     * For adaptive mutexes, spin for a bit in the expectation
     * that if the application requests this mutex type then
     * the lock is likely to be released quickly and it is
     * faster than entering the kernel
     */
    if (m_protocol == PthreadMutexProt::None) [[likely]] {
        s32 count = m_spinloops;
        while (count--) {
            if (m_lock.try_lock()) {
                m_owner = curthread;
                return 0;
            }
            CPU_SPINWAIT;
        }

        count = m_yieldloops;
        while (count--) {
            std::this_thread::yield();
            if (m_lock.try_lock()) {
                m_owner = curthread;
                return 0;
            }
        }
    }

    s32 ret = 0;
    const bool stats = MutexStatsPeriod() > 0.0;
    std::string owner_name;
    std::chrono::steady_clock::time_point wait_start;
    if (stats) {
        owner_name = ThreadName(m_owner);
        wait_start = std::chrono::steady_clock::now();
    }
    if (abstime == nullptr) {
        m_lock.lock();
    } else if (abstime != THR_RELTIME && (abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000))
        [[unlikely]] {
        ret = POSIX_EINVAL;
    } else {
        if (abstime == THR_RELTIME) {
            ret = m_lock.try_lock_for(std::chrono::microseconds(usec)) ? 0 : POSIX_ETIMEDOUT;
        } else {
            ret = m_lock.try_lock_until(abstime->TimePoint()) ? 0 : POSIX_ETIMEDOUT;
        }
    }
    if (ret == 0) {
        m_owner = curthread;
    }
    if (stats) {
        NoteMutexWait(this, owner_name, ThreadName(curthread),
                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - wait_start)
                          .count());
    }
    return ret;
}

s32 PthreadMutex::TryLock() {
    Pthread* curthread = g_curthread;
    if (m_owner == curthread) {
        return SelfTryLock();
    }
    const s32 ret = m_lock.try_lock() ? 0 : POSIX_EBUSY;
    if (ret == 0) {
        m_owner = curthread;
    }
    return ret;
}

s32 PS4_SYSV_ABI posix_pthread_mutex_trylock(PthreadMutexT* mutex) {
    CHECK_AND_INIT_MUTEX
    return (*mutex)->TryLock();
}

s32 PS4_SYSV_ABI posix_pthread_mutex_lock(PthreadMutexT* mutex) {
    CHECK_AND_INIT_MUTEX
    return (*mutex)->Lock(nullptr);
}

s32 PS4_SYSV_ABI posix_pthread_mutex_timedlock(PthreadMutexT* mutex,
                                               const OrbisKernelTimespec* abstime) {
    CHECK_AND_INIT_MUTEX
    return (*mutex)->Lock(abstime);
}

s32 PS4_SYSV_ABI posix_pthread_mutex_reltimedlock_np(PthreadMutexT* mutex, u64 usec) {
    CHECK_AND_INIT_MUTEX
    return (*mutex)->Lock(THR_RELTIME, usec);
}

s32 PthreadMutex::Unlock() {
    Pthread* curthread = g_curthread;
    /*
     * Check if the running thread is not the owner of the mutex.
     */
    if (m_owner != curthread) [[unlikely]] {
        return POSIX_EPERM;
    }

    if (Type() == PthreadMutexType::Recursive && m_count > 0) [[unlikely]] {
        m_count--;
    } else {
        const bool deferred = True(m_flags & PthreadMutexFlags::Deferred);
        m_flags &= ~PthreadMutexFlags::Deferred;

        m_owner = nullptr;
        m_lock.unlock();

        if (curthread->will_sleep == 0 && deferred) {
            curthread->WakeAll();
        }
    }
    return 0;
}

s32 PS4_SYSV_ABI posix_pthread_mutex_unlock(PthreadMutexT* mutex) {
    PthreadMutex* mp = *mutex;
    if (mp <= THR_MUTEX_DESTROYED) [[unlikely]] {
        if (mp == THR_MUTEX_DESTROYED) {
            return POSIX_EINVAL;
        }
        return POSIX_EPERM;
    }
    return mp->Unlock();
}

s32 PS4_SYSV_ABI posix_pthread_mutex_getspinloops_np(PthreadMutexT* mutex, int* count) {
    CHECK_AND_INIT_MUTEX
    *count = (*mutex)->m_spinloops;
    return 0;
}

s32 PS4_SYSV_ABI posix_pthread_mutex_setspinloops_np(PthreadMutexT* mutex, s32 count) {
    CHECK_AND_INIT_MUTEX(*mutex)->m_spinloops = count;
    return 0;
}

s32 PS4_SYSV_ABI posix_pthread_mutex_getyieldloops_np(PthreadMutexT* mutex, int* count) {
    CHECK_AND_INIT_MUTEX
    *count = (*mutex)->m_yieldloops;
    return 0;
}

s32 PS4_SYSV_ABI posix_pthread_mutex_setyieldloops_np(PthreadMutexT* mutex, s32 count) {
    CHECK_AND_INIT_MUTEX(*mutex)->m_yieldloops = count;
    return 0;
}

s32 PS4_SYSV_ABI posix_pthread_mutex_isowned_np(PthreadMutexT* mutex) {
    PthreadMutex* m = *mutex;
    if (m <= THR_MUTEX_DESTROYED) {
        return 0;
    }
    return m->m_owner == g_curthread;
}

s32 PthreadMutex::IsOwned(Pthread* curthread) const {
    if (this <= THR_MUTEX_DESTROYED) [[unlikely]] {
        if (this == THR_MUTEX_DESTROYED) {
            return POSIX_EINVAL;
        }
        return POSIX_EPERM;
    }
    if (m_owner != curthread) {
        return POSIX_EPERM;
    }
    return 0;
}

s32 PS4_SYSV_ABI posix_pthread_mutexattr_init(PthreadMutexAttrT* attr) {
    auto pattr = new (std::nothrow) PthreadMutexAttr{};
    if (pattr == nullptr) {
        return POSIX_ENOMEM;
    }
    memcpy(pattr, &PthreadMutexattrDefault, sizeof(PthreadMutexAttr));
    *attr = pattr;
    return 0;
}

s32 PS4_SYSV_ABI posix_pthread_mutexattr_setkind_np(PthreadMutexAttrT* attr,
                                                    PthreadMutexType kind) {
    if (attr == nullptr || *attr == nullptr) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    (*attr)->m_type = kind;
    return 0;
}

s32 PS4_SYSV_ABI posix_pthread_mutexattr_getkind_np(PthreadMutexAttrT attr) {
    if (attr == nullptr) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    return static_cast<int>(attr->m_type);
}

s32 PS4_SYSV_ABI posix_pthread_mutexattr_setprioceiling(PthreadMutexAttrT* attr, int prioceiling) {
    if (attr == nullptr || *attr == nullptr || (*attr)->m_protocol != PthreadMutexProt::Protect ||
        prioceiling > ORBIS_KERNEL_PRIO_FIFO_HIGHEST ||
        prioceiling < ORBIS_KERNEL_PRIO_FIFO_LOWEST) {
        return POSIX_EINVAL;
    }
    (*attr)->m_ceiling = prioceiling;
    return 0;
}

s32 PS4_SYSV_ABI posix_pthread_mutexattr_getprioceiling(PthreadMutexAttrT* attr, int* prioceiling) {
    if (attr == nullptr || *attr == nullptr || (*attr)->m_protocol != PthreadMutexProt::Protect) {
        return POSIX_EINVAL;
    }
    *prioceiling = (*attr)->m_ceiling;
    return 0;
}

s32 PS4_SYSV_ABI posix_pthread_mutexattr_setprotocol(PthreadMutexAttrT* mattr,
                                                     PthreadMutexProt protocol) {
    if (mattr == nullptr || *mattr == nullptr || (protocol < PthreadMutexProt::None) ||
        (protocol > PthreadMutexProt::Protect)) {
        return POSIX_EINVAL;
    }
    (*mattr)->m_protocol = protocol;
    (*mattr)->m_ceiling = ORBIS_KERNEL_PRIO_RR_HIGHEST;
    return 0;
}

s32 PS4_SYSV_ABI posix_pthread_mutexattr_getprotocol(PthreadMutexAttrT* mattr,
                                                     PthreadMutexProt* protocol) {
    if (mattr == nullptr || *mattr == nullptr) {
        return POSIX_EINVAL;
    }
    *protocol = (*mattr)->m_protocol;
    return 0;
}

s32 PS4_SYSV_ABI posix_pthread_mutexattr_setpshared(PthreadMutexAttrT* attr, s32 pshared) {
    constexpr s32 POSIX_PTHREAD_PROCESS_PRIVATE = 0;
    constexpr s32 POSIX_PTHREAD_PROCESS_SHARED = 1;
    if (!attr || !*attr || pshared != POSIX_PTHREAD_PROCESS_PRIVATE) {
        return POSIX_EINVAL;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI posix_pthread_mutexattr_getpshared(PthreadMutexAttrT* attr, s32* pshared) {
    if (!attr || !*attr) {
        return POSIX_EINVAL;
    }
    *pshared = 0;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI posix_pthread_mutexattr_settype(PthreadMutexAttrT* attr, PthreadMutexType type) {
    if (attr == nullptr || *attr == nullptr || type < PthreadMutexType::ErrorCheck ||
        type >= PthreadMutexType::Max) {
        return POSIX_EINVAL;
    }
    (*attr)->m_type = type;
    return 0;
}

s32 PS4_SYSV_ABI posix_pthread_mutexattr_gettype(PthreadMutexAttrT* attr, PthreadMutexType* type) {
    if (attr == nullptr || *attr == nullptr || (*attr)->m_type >= PthreadMutexType::Max) {
        return POSIX_EINVAL;
    }
    *type = (*attr)->m_type;
    return 0;
}

s32 PS4_SYSV_ABI posix_pthread_mutexattr_destroy(PthreadMutexAttrT* attr) {
    if (attr == nullptr || *attr == nullptr) {
        return POSIX_EINVAL;
    }
    delete *attr;
    *attr = nullptr;
    return 0;
}

void RegisterMutex(Core::Loader::SymbolsResolver* sym) {
    // Posix
    LIB_FUNCTION("ttHNfU+qDBU", "libScePosix", 1, "libkernel", posix_pthread_mutex_init);
    LIB_FUNCTION("gKqzW-zWhvY", "libScePosix", 1, "libkernel", posix_pthread_mutex_isowned_np);
    LIB_FUNCTION("7H0iTOciTLo", "libScePosix", 1, "libkernel", posix_pthread_mutex_lock);
    LIB_FUNCTION("Io9+nTKXZtA", "libScePosix", 1, "libkernel", posix_pthread_mutex_timedlock);
    LIB_FUNCTION("K-jXhbt2gn4", "libScePosix", 1, "libkernel", posix_pthread_mutex_trylock);
    LIB_FUNCTION("2Z+PpY6CaJg", "libScePosix", 1, "libkernel", posix_pthread_mutex_unlock);
    LIB_FUNCTION("x4vQj3JKKmc", "libScePosix", 1, "libkernel", posix_pthread_mutex_getspinloops_np);
    LIB_FUNCTION("OxEIUqkByy4", "libScePosix", 1, "libkernel",
                 posix_pthread_mutex_getyieldloops_np);
    LIB_FUNCTION("5-ncLMtL5+g", "libScePosix", 1, "libkernel", posix_pthread_mutex_setspinloops_np);
    LIB_FUNCTION("frFuGprJmPc", "libScePosix", 1, "libkernel",
                 posix_pthread_mutex_setyieldloops_np);
    LIB_FUNCTION("ltCfaGr2JGE", "libScePosix", 1, "libkernel", posix_pthread_mutex_destroy);
    LIB_FUNCTION("dQHWEsJtoE4", "libScePosix", 1, "libkernel", posix_pthread_mutexattr_init);
    LIB_FUNCTION("U6SNV+RnyLQ", "libScePosix", 1, "libkernel", posix_pthread_mutexattr_getkind_np);
    LIB_FUNCTION("+m8+quqOwhM", "libScePosix", 1, "libkernel",
                 posix_pthread_mutexattr_getprioceiling);
    LIB_FUNCTION("yDaWxUE50s0", "libScePosix", 1, "libkernel", posix_pthread_mutexattr_getprotocol);
    LIB_FUNCTION("PmL-TwKUzXI", "libScePosix", 1, "libkernel", posix_pthread_mutexattr_getpshared);
    LIB_FUNCTION("GZFlI7RhuQo", "libScePosix", 1, "libkernel", posix_pthread_mutexattr_gettype);
    LIB_FUNCTION("J9rlRuQ8H5s", "libScePosix", 1, "libkernel", posix_pthread_mutexattr_setkind_np);
    LIB_FUNCTION("ZLvf6lVAc4M", "libScePosix", 1, "libkernel",
                 posix_pthread_mutexattr_setprioceiling);
    LIB_FUNCTION("5txKfcMUAok", "libScePosix", 1, "libkernel", posix_pthread_mutexattr_setprotocol);
    LIB_FUNCTION("EXv3ztGqtDM", "libScePosix", 1, "libkernel", posix_pthread_mutexattr_setpshared);
    LIB_FUNCTION("mDmgMOGVUqg", "libScePosix", 1, "libkernel", posix_pthread_mutexattr_settype);
    LIB_FUNCTION("HF7lK46xzjY", "libScePosix", 1, "libkernel", posix_pthread_mutexattr_destroy);

    // Posix-Kernel
    LIB_FUNCTION("ttHNfU+qDBU", "libkernel", 1, "libkernel", posix_pthread_mutex_init);
    LIB_FUNCTION("gKqzW-zWhvY", "libkernel", 1, "libkernel", posix_pthread_mutex_isowned_np);
    LIB_FUNCTION("7H0iTOciTLo", "libkernel", 1, "libkernel", posix_pthread_mutex_lock);
    LIB_FUNCTION("Io9+nTKXZtA", "libkernel", 1, "libkernel", posix_pthread_mutex_timedlock);
    LIB_FUNCTION("K-jXhbt2gn4", "libkernel", 1, "libkernel", posix_pthread_mutex_trylock);
    LIB_FUNCTION("2Z+PpY6CaJg", "libkernel", 1, "libkernel", posix_pthread_mutex_unlock);
    LIB_FUNCTION("x4vQj3JKKmc", "libkernel", 1, "libkernel", posix_pthread_mutex_getspinloops_np);
    LIB_FUNCTION("OxEIUqkByy4", "libkernel", 1, "libkernel", posix_pthread_mutex_getyieldloops_np);
    LIB_FUNCTION("5-ncLMtL5+g", "libkernel", 1, "libkernel", posix_pthread_mutex_setspinloops_np);
    LIB_FUNCTION("frFuGprJmPc", "libkernel", 1, "libkernel", posix_pthread_mutex_setyieldloops_np);
    LIB_FUNCTION("ltCfaGr2JGE", "libkernel", 1, "libkernel", posix_pthread_mutex_destroy);
    LIB_FUNCTION("dQHWEsJtoE4", "libkernel", 1, "libkernel", posix_pthread_mutexattr_init);
    LIB_FUNCTION("U6SNV+RnyLQ", "libkernel", 1, "libkernel", posix_pthread_mutexattr_getkind_np);
    LIB_FUNCTION("+m8+quqOwhM", "libkernel", 1, "libkernel",
                 posix_pthread_mutexattr_getprioceiling);
    LIB_FUNCTION("yDaWxUE50s0", "libkernel", 1, "libkernel", posix_pthread_mutexattr_getprotocol);
    LIB_FUNCTION("PmL-TwKUzXI", "libkernel", 1, "libkernel", posix_pthread_mutexattr_getpshared);
    LIB_FUNCTION("GZFlI7RhuQo", "libkernel", 1, "libkernel", posix_pthread_mutexattr_gettype);
    LIB_FUNCTION("J9rlRuQ8H5s", "libkernel", 1, "libkernel", posix_pthread_mutexattr_setkind_np);
    LIB_FUNCTION("ZLvf6lVAc4M", "libkernel", 1, "libkernel",
                 posix_pthread_mutexattr_setprioceiling);
    LIB_FUNCTION("5txKfcMUAok", "libkernel", 1, "libkernel", posix_pthread_mutexattr_setprotocol);
    LIB_FUNCTION("EXv3ztGqtDM", "libkernel", 1, "libkernel", posix_pthread_mutexattr_setpshared);
    LIB_FUNCTION("mDmgMOGVUqg", "libkernel", 1, "libkernel", posix_pthread_mutexattr_settype);
    LIB_FUNCTION("HF7lK46xzjY", "libkernel", 1, "libkernel", posix_pthread_mutexattr_destroy);

    // Orbis
    LIB_FUNCTION("cmo1RIYva9o", "libkernel", 1, "libkernel", ORBIS(scePthreadMutexInit));
    LIB_FUNCTION("qH1gXoq71RY", "libkernel", 1, "libkernel", ORBIS(posix_pthread_mutex_init));
    LIB_FUNCTION("W6OrTBO95UY", "libkernel", 1, "libkernel", ORBIS(posix_pthread_mutex_isowned_np));
    LIB_FUNCTION("9UK1vLZQft4", "libkernel", 1, "libkernel", ORBIS(posix_pthread_mutex_lock));
    LIB_FUNCTION("IafI2PxcPnQ", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_mutex_reltimedlock_np));
    LIB_FUNCTION("upoVrzMHFeE", "libkernel", 1, "libkernel", ORBIS(posix_pthread_mutex_trylock));
    LIB_FUNCTION("tn3VlD0hG60", "libkernel", 1, "libkernel", ORBIS(posix_pthread_mutex_unlock));
    LIB_FUNCTION("pOmNmyRKlIE", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_mutex_getspinloops_np));
    LIB_FUNCTION("AWS3NyViL9o", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_mutex_getyieldloops_np));
    LIB_FUNCTION("42YkUouoMI0", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_mutex_setspinloops_np));
    LIB_FUNCTION("bP+cqFmBW+A", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_mutex_setyieldloops_np));
    LIB_FUNCTION("2Of0f+3mhhE", "libkernel", 1, "libkernel", ORBIS(posix_pthread_mutex_destroy));
    LIB_FUNCTION("n2MMpvU8igI", "libkernel", 1, "libkernel", ORBIS(posix_pthread_mutexattr_init));
    LIB_FUNCTION("F8bUHwAG284", "libkernel", 1, "libkernel", ORBIS(posix_pthread_mutexattr_init));
    LIB_FUNCTION("rH2mWEndluc", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_mutexattr_getkind_np));
    LIB_FUNCTION("SgjMpyH9Z9I", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_mutexattr_getprioceiling));
    LIB_FUNCTION("GoTmFeui+hQ", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_mutexattr_getprotocol));
    LIB_FUNCTION("losEubHc64c", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_mutexattr_getpshared));
    LIB_FUNCTION("gquEhBrS2iw", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_mutexattr_gettype));
    LIB_FUNCTION("UWZbVSFze24", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_mutexattr_setkind_np));
    LIB_FUNCTION("532IaQguwMg", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_mutexattr_setprioceiling));
    LIB_FUNCTION("1FGvU0i9saQ", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_mutexattr_setprotocol));
    LIB_FUNCTION("mxKx9bxXF2I", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_mutexattr_setpshared));
    LIB_FUNCTION("iMp8QpE+XO4", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_mutexattr_settype));
    LIB_FUNCTION("smWEktiyyG0", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_mutexattr_destroy));
}

} // namespace Libraries::Kernel
