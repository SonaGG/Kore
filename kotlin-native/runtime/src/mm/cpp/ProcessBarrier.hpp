/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#pragma once

#include <atomic>

#include "Utils.hpp"

namespace kotlin::mm {

namespace internal {
extern bool gProcessBarrierAvailable;
} // namespace internal

// Kore: asymmetric fences (as in HotSpot and .NET). A thread switching between the Runnable and Native states (around
// every call to native code) publishes its state with a plain store and reads the suspension request with a plain
// load, without a full fence; instead, the GC issues a process-wide barrier between requesting a suspension (or a
// safe point action) and reading the threads' states. The barrier makes every thread drain its store buffer, so either
// the GC sees a thread back in the Runnable state, or the thread sees the request. This makes native calls about as
// cheap as calls between native libraries. Platforms without a process-wide barrier keep full fences.
ALWAYS_INLINE inline bool processBarrierAvailable() noexcept {
#if KONAN_WINDOWS
    return true;
#else
    return internal::gProcessBarrierAvailable;
#endif
}

// Executes a full memory barrier on every thread of the process (or nothing, if `processBarrierAvailable()` is false).
void processWideBarrier() noexcept;

} // namespace kotlin::mm
