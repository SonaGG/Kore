/*
 * Copyright 2010-2023 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cinttypes>
#include <thread>

#include "Utils.hpp"

namespace kotlin {

// `std::condition_variable_any` implemented via spinning on atomics.
class ConditionVariableSpin : private Pinned {
public:
    // Notify all threads waiting for `this.
    // Exactly the same as `notify_all`. Providing this method to be fully compatible
    // with `std::condition_variable_any`.
    void notify_one() noexcept {
        // Conditional variable does not protect the data: a mutex must be
        // used to protect it, so we don't need synchronization.
        epoch_.fetch_add(1, std::memory_order_relaxed);
    }

    // Notify all threads waiting for `this.
    void notify_all() noexcept {
        // Conditional variable does not protect the data: a mutex must be
        // used to protect it, so we don't need synchronization.
        epoch_.fetch_add(1, std::memory_order_relaxed);
    }

    // Wait until next call to `notify_*`.
    template <typename Lock>
    void wait(Lock& lock) {
        auto currentEpoch = epoch_.load(std::memory_order_relaxed);
        lock.unlock();
        // Waiting for any change of the epoch.
        while (epoch_.load(std::memory_order_relaxed) == currentEpoch) {
            std::this_thread::yield();
        }
        lock.lock();
    }

    // Waits until `stopWaiting()` starts returning true. Any change
    // that leads to `stopWaiting()` must be followed by `notify_*` for
    // this `wait` to stop waiting.
    template <typename Lock, typename Predicate>
    void wait(Lock& lock, Predicate stopWaiting) {
        while (!stopWaiting()) {
            wait(lock);
        }
    }

    // TODO: Implement wait_until and wait_for using kotlin::steady_clock.

private:
    std::atomic<size_t> epoch_ = 0;
};

// Kore: waits on `cv` (with `lock` held) until `pred` holds, checking it again every 2 ms. MinGW's condition variables
// (winpthreads) can lose a wake-up under contention: a waiter would then sleep forever although its condition holds.
template <typename Lock, typename Predicate>
void waitRobustly(std::condition_variable& cv, Lock& lock, Predicate pred) {
    while (!pred()) cv.wait_for(lock, std::chrono::milliseconds(2));
}

template <typename Lock, typename Predicate>
void waitRobustly(std::condition_variable_any& cv, Lock& lock, Predicate pred) {
    while (!pred()) cv.wait_for(lock, std::chrono::milliseconds(2));
}

// Other condition variables (e.g. `ConditionVariableSpin`, which polls anyway).
template <typename ConditionVariable, typename Lock, typename Predicate>
void waitRobustly(ConditionVariable& cv, Lock& lock, Predicate pred) {
    cv.wait(lock, pred);
}

// Kore: one wait on `cv` (with `lock` held), as `cv.wait(lock)` for a caller that checks its condition itself, but for
// at most 2 ms (see `waitRobustly`).
template <typename Lock>
void waitOnceRobustly(std::condition_variable& cv, Lock& lock) {
    cv.wait_for(lock, std::chrono::milliseconds(2));
}

} // namespace kotlin
