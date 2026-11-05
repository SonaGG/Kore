/*
 * Copyright 2010-2021 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#include "ThreadData.hpp"
#include "ThreadSuspension.hpp"

#if KONAN_WINDOWS
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <condition_variable>

#include "CallsChecker.hpp"
#include "Logging.hpp"
#include "Porting.h"
#include "ProcessBarrier.hpp"
#include "SafePoint.hpp"

using namespace kotlin;

namespace {

[[clang::no_destroy]] thread_local std::optional<mm::SafePointActivator> gSafePointActivator = std::nullopt;
[[clang::no_destroy]] std::mutex gSuspensionRequestMutex;
[[clang::no_destroy]] std::condition_variable gSuspensionCondVar;

} // namespace

std::atomic<mm::internal::SuspensionReason> mm::internal::gSuspensionRequestReason = nullptr;

PERFORMANCE_INLINE mm::ThreadSuspensionData::MutatorPauseHandle::MutatorPauseHandle(const char* reason, mm::ThreadData& threadData) noexcept
    : reason_(reason), threadData_(threadData), pauseStartTimeMicros_(konan::getTimeMicros())
{
    auto prevState = threadData_.suspensionData().setStateNoSafePoint(ThreadState::kNative);
    // no special reason, fill free to implement pause from native if needed
    RuntimeAssert(prevState == ThreadState::kRunnable, "Expected runnable state");
    RuntimeLogDebug({logging::Tag::kPause}, "Suspending mutation (%s)", reason_);
}

PERFORMANCE_INLINE mm::ThreadSuspensionData::MutatorPauseHandle::~MutatorPauseHandle() noexcept {
    if (!resumed) resume();
}

PERFORMANCE_INLINE void mm::ThreadSuspensionData::MutatorPauseHandle::resume() noexcept {
    RuntimeAssert(!resumed, "Must not be resumed yet");
    auto prevState = threadData_.suspensionData().setStateNoSafePoint(ThreadState::kRunnable);
    RuntimeAssert(prevState == ThreadState::kNative, "Expected native state");
    auto pauseTimeMicros = konan::getTimeMicros() - pauseStartTimeMicros_;
    RuntimeLogInfo({logging::Tag::kPause}, "Resuming mutation after %" PRIu64 " microseconds of suspension (%s)", pauseTimeMicros, reason_);
    resumed = true;
}

kotlin::ThreadState kotlin::mm::ThreadSuspensionData::setState(kotlin::ThreadState newState) noexcept {
    // Kore: the stack the GC scans while this thread is not runnable (see `saveStackPointer`).
    if (newState == ThreadState::kNative) saveStateForStackScanning();
    if (processBarrierAvailable()) {
        // Kore: only this thread writes its state. See `processBarrierAvailable`; the GC issues `processWideBarrier`
        // before it reads the state, after requesting a suspension or a safe point action.
        ThreadState oldState = state_.load(std::memory_order_relaxed);
        state_.store(newState, std::memory_order_release);
        if (oldState == ThreadState::kNative && newState == ThreadState::kRunnable) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
            safePoint(threadData_, std::memory_order_acquire);
        }
        return oldState;
    }
    ThreadState oldState = state_.exchange(newState);
    if (oldState == ThreadState::kNative && newState == ThreadState::kRunnable) {
        // Must use already acquired `ThreadData` because TLS may be in invalid state e.g. during thread detach.
        // Also, this must load SP in sequentially consistent order, because GC
        // may have touched this thread's data, and we must synchronize before
        // continuing.
        // GC would have either changed stored SP handler (with seq_cst),
        // or would have changed `internal::gSuspensionRequested` (with seq_cst),
        // so, loading SP here, or checking `internal::gSuspensionRequested` in
        // `suspendIfRequested` is enough.
        safePoint(threadData_, std::memory_order_seq_cst);
    }
    return oldState;
}

NO_EXTERNAL_CALLS_CHECK void kotlin::mm::ThreadSuspensionData::suspendIfRequested() noexcept {
    if (IsThreadSuspensionRequested()) {
        auto pauseHandle = pauseMutationInScope(internal::gSuspensionRequestReason.load(std::memory_order_relaxed));

        threadData_.gc().OnSuspendForGC();
        std::unique_lock lock(gSuspensionRequestMutex);
        gSuspensionCondVar.wait(lock, []() { return !IsThreadSuspensionRequested(); });

        // Must return to running state under the lock.
        pauseHandle.resume();
    }
}

void mm::ThreadSuspensionData::requestThreadsSuspension(const char* reason) noexcept {
    RuntimeAssert(state() == ThreadState::kRunnable, "Requesting thread suspension from the Native state may lead to a deadlock");

    while (!TryRequestThreadsSuspension(reason)) {
        mm::safePoint(threadData_);
    }
}

PERFORMANCE_INLINE mm::ThreadSuspensionData::MutatorPauseHandle mm::ThreadSuspensionData::pauseMutationInScope(const char* reason) noexcept {
    return MutatorPauseHandle(reason, threadData_);
}

void kotlin::mm::RequestThreadsSuspension(internal::SuspensionReason reason) noexcept {
    RuntimeAssert(!mm::ThreadRegistry::Instance().IsCurrentThreadRegistered(),
                  "Registered thread must properly handle concurrent suspension requests (suspend if requested)");

    while (!TryRequestThreadsSuspension(reason)) {
        std::unique_lock lock(gSuspensionRequestMutex);
        gSuspensionCondVar.wait(lock, []() { return !IsThreadSuspensionRequested(); });
    }
}

bool kotlin::mm::TryRequestThreadsSuspension(internal::SuspensionReason reason) noexcept {
    CallsCheckerIgnoreGuard guard;

    RuntimeAssert(gSafePointActivator == std::nullopt, "Current thread already suspended threads.");
    {
        std::unique_lock lock(gSuspensionRequestMutex);
        // Someone else has already suspended threads.
        if (internal::gSuspensionRequestReason.load(std::memory_order_relaxed) != nullptr) {
            return false;
        }
        // First enable safepoints.
        gSafePointActivator = mm::SafePointActivator();
        // At this point, the mutators are starting to enter safepoint slow paths, but they are not getting
        // suspended there and continue executing user code.
        // Now request the threads suspension.
        internal::gSuspensionRequestReason.store(reason);
        // The mutators will now start to suspend on their next encountered safepoints.
    }

    return true;
}

void kotlin::mm::WaitForThreadsSuspension() noexcept {
    // Kore: see `processBarrierAvailable`.
    processWideBarrier();
    auto& threadRegistry = kotlin::mm::ThreadRegistry::Instance();
    auto* currentThread = (threadRegistry.IsCurrentThreadRegistered()) ? threadRegistry.CurrentThreadData() : nullptr;
    // Spin waiting for threads to suspend. Ignore Native threads.
    threadRegistry.waitAllThreads([currentThread](mm::ThreadData& thread) noexcept {
        return &thread == currentThread || thread.suspensionData().suspendedOrNative();
    });
}

void kotlin::mm::ResumeThreads() noexcept {
    RuntimeAssert(gSafePointActivator != std::nullopt, "Current thread must have suspended threads");
    gSafePointActivator = std::nullopt;

    // From the std::condition_variable docs:
    // Even if the shared variable is atomic, it must be modified under
    // the mutex in order to correctly publish the modification to the waiting thread.
    // https://en.cppreference.com/w/cpp/thread/condition_variable
    {
        std::unique_lock lock(gSuspensionRequestMutex);
        internal::gSuspensionRequestReason.store(nullptr);
    }
    gSuspensionCondVar.notify_all();
}

uintptr_t kotlin::mm::ThreadSuspensionData::currentThreadStackBase() noexcept {
#if KONAN_WINDOWS
    return reinterpret_cast<uintptr_t>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
#elif KONAN_APPLE
    return reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
#elif KONAN_LINUX || KONAN_ANDROID
    pthread_attr_t attributes;
    void* address = nullptr;
    size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &attributes) == 0) {
        pthread_attr_getstack(&attributes, &address, &size);
        pthread_attr_destroy(&attributes);
    }
    return reinterpret_cast<uintptr_t>(address) + size;
#else
    return 0; // Unknown: no conservative stack scanning.
#endif
}

// Kore: a call, so that the registers hold the caller's values (those live across the call are in callee-saved
// registers or in memory). The asm only uses a caller-saved register.
NO_INLINE void kotlin::mm::ThreadSuspensionData::captureRegistersAndStackPointer() noexcept {
#if defined(__x86_64__)
    asm volatile(
            "movq %%rbx, 0(%0)\n\t"
            "movq %%rbp, 8(%0)\n\t"
            "movq %%rdi, 16(%0)\n\t"
            "movq %%rsi, 24(%0)\n\t"
            "movq %%r12, 32(%0)\n\t"
            "movq %%r13, 40(%0)\n\t"
            "movq %%r14, 48(%0)\n\t"
            "movq %%r15, 56(%0)\n\t" ::"a"(savedRegisters_)
            : "memory");
#elif defined(__aarch64__)
    register uintptr_t* registers asm("x9") = savedRegisters_;
    asm volatile(
            "stp x19, x20, [%0, #0]\n\t"
            "stp x21, x22, [%0, #16]\n\t"
            "stp x23, x24, [%0, #32]\n\t"
            "stp x25, x26, [%0, #48]\n\t"
            "stp x27, x28, [%0, #64]\n\t" ::"r"(registers)
            : "memory");
#endif
    saveStackPointer();
}
