/*
 * Copyright 2010-2024 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#include "ConcurrentMark.hpp"
#include "ProcessBarrier.hpp"

#include <algorithm>
#include <thread>

#include "MarkAndSweepUtils.hpp"
#include "GCStatistics.hpp"
#include "GCImpl.hpp"

using namespace kotlin;

namespace {

class FlushActionActivator final : public mm::ExtraSafePointActionActivator<FlushActionActivator> {};

} // namespace

void gc::mark::ConcurrentMark::ThreadData::onSuspendForGC() noexcept {}

bool gc::mark::ConcurrentMark::ThreadData::tryLockRootSet() noexcept {
    bool expected = false;
    bool locked = rootSetLocked_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    if (locked) {
        RuntimeLogDebug(
                {kTagGC}, "Thread %" PRIuPTR " have exclusively acquired thread %" PRIuPTR "'s root set", konan::currentThreadId(),
                threadData_.threadId());
    }
    return locked;
}

void gc::mark::ConcurrentMark::ThreadData::publish() noexcept {
    threadData_.Publish();
}

void gc::mark::ConcurrentMark::ThreadData::clearMarkFlags() noexcept {
    rootSetLocked_.store(false, std::memory_order_release);
}

void gc::mark::ConcurrentMark::ThreadData::ensureFlushActionExecuted() noexcept {
    flushAction_->ensureExecuted([this] { markQueue()->forceFlush(); });
}

void gc::mark::ConcurrentMark::ThreadData::onSafePoint() noexcept {
    FlushActionActivator::doIfActive([this] { ensureFlushActionExecuted(); });
}

void gc::mark::ConcurrentMark::setupBeforeSTW(GCHandle gcHandle) {
    gcHandle_ = gcHandle;

    lockedMutatorsList_ = mm::ThreadRegistry::Instance().LockForIter();

    parallelProcessor_.construct();
}

void gc::mark::ConcurrentMark::endMarkingEpoch() {
    parallelProcessor_.destroy();
    resetMutatorFlags();
    lockedMutatorsList_ = std::nullopt;
}

void gc::mark::ConcurrentMark::markInSTW() {
    std::unique_lock markLock(markMutex_);
    ParallelProcessor::Worker mainWorker(*parallelProcessor_);
    GCLogDebug(gcHandle().getEpoch(), "Creating main (#0) mark worker");

    // create mutator mark queues
    for (auto& thread : *lockedMutatorsList_) {
        thread.gc().impl().mark_.markQueue().construct(*parallelProcessor_);
    }

    completeMutatorsRootSet(mainWorker);

    barriers::enableBarriers(gcHandle().getEpoch());
    resumeTheWorld(gcHandle());

    // global root set must be collected after all the mutator's global data have been published
    collectRootSetGlobals<MarkTraits>(gcHandle(), mainWorker);

    // Mutator threads might release their internal batch at a pretty arbitrary moment (during a barrier execution with overflow).
    // So there are not so many reliable ways to track releases of new work.
    // The number of batches shared inside a parallel processor may only grow,
    // we use this number to decide when to finish the mark.
    auto everSharedBatches = parallelProcessor_->batchesEverShared();

    bool terminateInSTW = true;
    for (uint32_t iter = 0; iter < compiler::concurrentMarkMaxIterations(); ++iter) {
        GCLogDebug(gcHandle().getEpoch(), "Building mark closure (attempt #%u)", iter);
        Mark<MarkTraits>(gcHandle(), mainWorker);
        if (tryTerminateMark(everSharedBatches)) {
            terminateInSTW = false; // successfully terminated mark phase concurrently
            break;
        }
    }
    if (terminateInSTW) {
        GCLogWarning(gcHandle().getEpoch(), "Finishing mark closure in STW after %u concurrent attempts",
            compiler::concurrentMarkMaxIterations());
        stopTheWorld(gcHandle(), "GC stop the world: concurrent mark took too long");
        flushMutatorQueues(); // No need for mark termination lock: STW is stronger than it
        Mark<MarkTraits>(gcHandle(), mainWorker);
        // Weak processing expects barriers in the correct state even in STW
        barriers::switchToWeakProcessingBarriers();
    }

    // By this point mutator mark queues cannot be populated anymore.
    // However, some threads may still try to enqueue a marked object before they observe the barriers were disabled.
    // Thus, mark queue destruction takes place only later below (in STW).

    gc::processWeaks<DefaultProcessWeaksTraits>(gcHandle(), mm::ExternalRCRefRegistry::instance());

    if (!terminateInSTW) {
        stopTheWorld(gcHandle(), "GC stop the world: prepare to sweep");
    }

    barriers::disableBarriers();

    for (auto& thread : *lockedMutatorsList_) {
        thread.gc().impl().mark_.markQueue().destroy();
    }
    endMarkingEpoch();
}

namespace {

// Kore: number of helper threads for a young collection's mark, besides the GC thread.
size_t youngMarkHelperCount() noexcept {
    // `KORE_GC_HELPERS` overrides it, for experiments.
    if (const char* helpers = std::getenv("KORE_GC_HELPERS")) return static_cast<size_t>(std::atoi(helpers));
    auto hardwareThreads = std::thread::hardware_concurrency();
    if (hardwareThreads <= 2) return 0;
    return std::min<size_t>(hardwareThreads / 2 - 1, 3);
}

} // namespace

gc::mark::ConcurrentMark::~ConcurrentMark() {
    requestShutdown();
}

void gc::mark::ConcurrentMark::requestShutdown() {
    {
        std::unique_lock lock(helpersMutex_);
        helpersShutdown_ = true;
    }
    helpersStart_.notify_all();
    helpers_.clear(); // joins
}

void gc::mark::ConcurrentMark::helperBody() noexcept {
    uint64_t seenRound = 0;
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(helpersMutex_);
            helpersStart_.wait(lock, [&] { return helpersShutdown_ || helpersRound_ != seenRound; });
            if (helpersShutdown_) return;
            seenRound = helpersRound_;
            task = helpersTask_;
        }
        task();
        {
            std::unique_lock lock(helpersMutex_);
            if (--helpersRunning_ == 0) helpersDone_.notify_all();
        }
    }
}

void gc::mark::ConcurrentMark::startHelpers(std::function<void()> task) noexcept {
    if (helpers_.empty()) {
        for (size_t i = 0, count = youngMarkHelperCount(); i < count; ++i) {
            helpers_.emplace_back(ScopedThread::attributes().name("GC helper"), [this] { helperBody(); });
        }
    }
    if (helpers_.empty()) return;
    {
        std::unique_lock lock(helpersMutex_);
        helpersTask_ = std::move(task);
        helpersRunning_ = helpers_.size();
        ++helpersRound_;
    }
    helpersStart_.notify_all();
}

void gc::mark::ConcurrentMark::waitHelpers() noexcept {
    std::unique_lock lock(helpersMutex_);
    helpersDone_.wait(lock, [&] { return helpersRunning_ == 0; });
    helpersTask_ = nullptr;
}

void gc::mark::ConcurrentMark::runInParallel(const std::function<void()>& task) noexcept {
    startHelpers(task);
    task();
    waitHelpers();
}

void gc::mark::ConcurrentMark::markYoungInSTW(
        GCHandle gcHandle,
        const std::function<void(ParallelProcessor::Worker&, std::vector<ObjHeader**>&)>& markRememberedSets) {
    std::unique_lock markLock(markMutex_);
    parallelProcessor_.construct();
    parallelProcessor_->setExternalWork(&internal::markArraySlicesPending);
    {
        ParallelProcessor::Worker worker(*parallelProcessor_);
        // Helpers register their own workers and take batches the GC thread shares while it collects roots and marks.
        // A helper that arrives after the mark has terminated just finds no work.
        startHelpers([this, gcHandle] {
            ParallelProcessor::Worker helperWorker(*parallelProcessor_);
            Mark<MarkTraits>(gcHandle, helperWorker);
        });
        // As `collectRootSet`, but the conservative stack candidates of all threads are looked up with the remembered
        // set's slots.
        MarkTraits::clear(worker);
        std::vector<ObjHeader**> stackCandidates;
        for (auto& thread : mm::GlobalData::Instance().threadRegistry().LockForIter()) {
            thread.Publish();
            collectRootSetForThread<MarkTraits>(gcHandle, worker, thread, &stackCandidates);
        }
        collectRootSetGlobals<MarkTraits>(gcHandle, worker);
        markRememberedSets(worker, stackCandidates);
        Mark<MarkTraits>(gcHandle, worker);
    }
    waitHelpers();
    gc::processWeaks<DefaultProcessWeaksTraits>(gcHandle, mm::ExternalRCRefRegistry::instance());
    parallelProcessor_.destroy();
}

gc::GCHandle& gc::mark::ConcurrentMark::gcHandle() {
    RuntimeAssert(gcHandle_.isValid(), "GCHandle must be initialized");
    return gcHandle_;
}

void gc::mark::ConcurrentMark::completeMutatorsRootSet(MarkTraits::MarkQueue& markQueue) {
    // workers compete for mutators to collect their root set
    for (auto& thread : *lockedMutatorsList_) {
        tryCollectRootSet(thread, markQueue);
    }
}

void gc::mark::ConcurrentMark::tryCollectRootSet(mm::ThreadData& thread, MarkTraits::MarkQueue& markQueue) {
    auto& gcData = thread.gc().impl().mark_;
    if (!gcData.tryLockRootSet()) return;

    GCLogDebug(gcHandle().getEpoch(), "Root set collection on thread %" PRIuPTR " for thread %" PRIuPTR, konan::currentThreadId(), thread.threadId());
    gcData.publish();
    collectRootSetForThread<MarkTraits>(gcHandle(), markQueue, thread);
}

/** Terminates the mark loop if possible, otherwise returns `false`. */
bool gc::mark::ConcurrentMark::tryTerminateMark(std::size_t& everSharedBatches) noexcept {
    // prevent unwanted mutations (such as weak-reachable resurrection) during termination detection
    std::unique_lock markTerminationGuard(markTerminationMutex_);

    // has to happen under the termination lock guard
    flushMutatorQueues();

    // After the mutators have been forced to flush their local queues,
    // there is only one possibility for this counter to remain the same as on a previous iteration:
    // 1. Mutator local queues are empty,
    // 2. AND were empty before the flush request was made,
    // 3. AND the last attempt at completing mark closure encountered 0 new objects // FIXME this is actually redundant
    const auto nowSharedBatches = parallelProcessor_->batchesEverShared();
    if (nowSharedBatches > everSharedBatches) {
        everSharedBatches = nowSharedBatches;
        parallelProcessor_->resetForNewWork();
        return false;
    }
    RuntimeAssert(nowSharedBatches == everSharedBatches, "This number must never decrease");

    barriers::switchToWeakProcessingBarriers();
    return true;
}

void gc::mark::ConcurrentMark::flushMutatorQueues() noexcept {
    for (auto& mutator : *lockedMutatorsList_) {
        mutator.gc().impl().mark_.flushAction_.construct();
    }

    {
        FlushActionActivator flushActivator{};
        // Kore: see `mm::processBarrierAvailable`.
        mm::processWideBarrier();

        // wait all mutators flushed
        while (true) {
            bool allDone = true;
            for (auto& mutator : *lockedMutatorsList_) {
                auto& markData = mutator.gc().impl().mark_;
                if (mutator.suspensionData().suspendedOrNative()) {
                    markData.ensureFlushActionExecuted();
                } else if (!markData.flushAction_->executed()) {
                    allDone = false;
                }
            }
            if (allDone) break;
            std::this_thread::yield();
        }
    }

    // It's guaranteed by the activator that no mutator thread would access somethingFlushed_ at this point.
    for (auto& mutator : *lockedMutatorsList_) {
        mutator.gc().impl().mark_.flushAction_.destroy();
    }
}

void gc::mark::ConcurrentMark::resetMutatorFlags() {
    for (auto& mut : *lockedMutatorsList_) {
        mut.gc().impl().mark_.clearMarkFlags();
    }
}

bool gc::mark::test_support::flushActionRequested() {
    return FlushActionActivator::isActive();
}
