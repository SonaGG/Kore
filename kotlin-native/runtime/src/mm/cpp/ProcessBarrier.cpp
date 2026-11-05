/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#include "ProcessBarrier.hpp"

#if KONAN_WINDOWS
#include <windows.h>
#elif KONAN_LINUX
#include <linux/membarrier.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace kotlin;

namespace {

bool initProcessBarrier() noexcept {
#if KONAN_WINDOWS
    return true;
#elif KONAN_LINUX && defined(__NR_membarrier)
    int supported = static_cast<int>(syscall(__NR_membarrier, MEMBARRIER_CMD_QUERY, 0, 0));
    if (supported < 0 || !(supported & MEMBARRIER_CMD_PRIVATE_EXPEDITED)) return false;
    return syscall(__NR_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0) == 0;
#else
    return false;
#endif
}

} // namespace

// Decided before any Kotlin code runs, and never changes.
bool mm::internal::gProcessBarrierAvailable = initProcessBarrier();

void mm::processWideBarrier() noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (!processBarrierAvailable()) return;
#if KONAN_WINDOWS
    FlushProcessWriteBuffers();
#elif KONAN_LINUX && defined(__NR_membarrier)
    syscall(__NR_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0);
#endif
    std::atomic_thread_fence(std::memory_order_seq_cst);
}
