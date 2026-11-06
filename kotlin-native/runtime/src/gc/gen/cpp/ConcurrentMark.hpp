/*
 * Copyright 2010-2024 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <vector>

#include "GCStatistics.hpp"
#include "ManuallyScoped.hpp"
#include "MarkStack.hpp"
#include "ObjectData.hpp"
#include "concurrent/ParallelProcessor.hpp"
#include "SafePoint.hpp"
#include "ThreadData.hpp"
#include "ThreadRegistry.hpp"
#include "Utils.hpp"
#include "concurrent/Once.hpp"
#include "concurrent/ScopedThread.hpp"


namespace kotlin::gc::internal {
// Kore: during a young collection with a nursery, the order in which the mark reaches nursery objects (see
// `GenerationalGC::evacuateNursery`, which copies survivors in that order, so that objects used together end up
// together).
extern std::atomic<bool> gRecordNurseryOrder;
void recordNurseryOrder(ObjHeader* object) noexcept;
} // namespace kotlin::gc::internal

namespace kotlin::gc::mark {

/**
 * Implementation of a Mark GC phase, that runs concurrent with mutator threads, using a "Snapshot at the Beginning" approach.
 * Steps:
 * 1. Pause all mutators, collect their root sets.
 * 2. Mutators resume, maintaining a weak tri-collor invariant with the help of:
 *    - Deletion write barrier (Dijkstra et al):
 *      Remembers overwritten values in a thread-local mark queue.
 *      Barrier+write combination doesn't need to be atomic,
 *      as only references from mark phase beginning matter for the SatB approach.
 *    - Read barrier for weak refs:
 *      Remembers each object read via a weak reference in a thread-local mark queue.
 *      Prevents the possibility of inserting a strong reference to a weakly-reachable object behind the mark front.
 * 3. Concurrently, the marker thread builds a mark closure.
 *    From time to time the mutator threads flush their mark queues into the global one.
 * 4. In case the mark closure is complete, replace the remembering weak read barrier with
 *    the barrier that hides unmarked (dead) referents.
 * 5. Process and clean weak references.
 * 6. Pause mutators once again and disable all the barreirs.
 * 7. Prepare marked heap for sweeping and resume mutation. // TODO prepare heap without a pause
 */
class ConcurrentMark : private Pinned {
    // Kore: arrays of object pointers instead of a list linked through the objects' GC words.
    using MarkStackImpl = ChunkedStack<ObjHeader, 512>;

public:
    // work balancing parameters were chosen pretty arbitrary
    using ParallelProcessor = ParallelProcessor<MarkStackImpl, 512, 4096>;
    using MutatorQueue = ParallelProcessor::WorkSource;

    class MarkTraits {
    public:
        NO_INLINE static void reportUnconstructed(ObjHeader* object) noexcept;
        NO_INLINE static void checkEnqueue(ObjHeader* object) noexcept;
        static bool checking() noexcept;
        using MarkQueue = ParallelProcessor::Worker;
        using AnyQueue = ParallelProcessor::WorkSource;

        static constexpr auto kAllowHeapToStackRefs = false;

        ALWAYS_INLINE static void clear(AnyQueue& queue) noexcept { RuntimeAssert(queue.localEmpty(), "Mark queue must be empty"); }

        // Kore: slices of big arrays to mark (see `GC::processArrayInMark`), the external work of the parallel
        // processor: any mark worker takes them before looking for other work, and instead of waiting for it.
        static PERFORMANCE_INLINE bool arraySlicesPending() noexcept {
            return internal::markArraySlicesPending.load(std::memory_order_relaxed) != 0;
        }
        NO_INLINE static void markArraySlices(MarkQueue& queue) noexcept;

        static PERFORMANCE_INLINE ObjHeader* tryDequeue(MarkQueue& queue) noexcept {
            while (true) {
                if (__builtin_expect(arraySlicesPending(), false)) markArraySlices(queue);
                auto* object = queue.tryPop();
                if (object) {
                    RuntimeLogDebug({logging::Tag::kGCMark}, "Dequeued %p", object);
                    return object;
                }
                // Null: the mark is done, or there were slices to take (possibly taken by other workers since: then
                // look for work again, as returning would leave the processor waiting for this worker).
                if (queue.terminated()) return nullptr;
            }
        }

        static PERFORMANCE_INLINE bool tryEnqueue(AnyQueue& queue, ObjHeader* object) noexcept {
            auto& objectData = alloc::objectDataForObject(object);
            // Kore: a young collection stops at the old generation.
            if (internal::isYoungCollection()) {
                if (objectData.old()) return false;
                // Kore: objects without references (boxes, strings, primitive arrays) and without extra object data
                // need no tracing: mark them without a trip through the queue, which halves the work of marking many
                // small objects. Only in young collections, where the world is stopped (no extra object data appears
                // concurrently).
                auto* typeInfo = clearPointerBits(object->typeInfoOrMetaRelaxed(), OBJECT_TAG_MASK);
                if (typeInfo->typeInfo_ == typeInfo && typeInfo->processObjectInMark == Kotlin_processEmptyObjectInMark) {
                    bool marked = objectData.tryMark();
                    if (marked && __builtin_expect(internal::gRecordNurseryOrder.load(std::memory_order_relaxed), false)) {
                        internal::recordNurseryOrder(object);
                    }
                    return marked;
                }
            }
            // Kore: whoever sets the mark bit pushes the object.
            if (!objectData.tryMark()) return false;
            queue.tryPush(*object);
            if (__builtin_expect(internal::gRecordNurseryOrder.load(std::memory_order_relaxed), false)) {
                internal::recordNurseryOrder(object);
            }
            RuntimeLogDebug({logging::Tag::kGCMark}, "Enqueued %p", object);
            return true;
        }

        static PERFORMANCE_INLINE bool tryMark(ObjHeader* object) noexcept {
            auto& objectData = alloc::objectDataForObject(object);
            if (internal::isYoungCollection() && objectData.old()) return false; // Kore
            bool pushed = objectData.tryMark();
            if (pushed) {
                RuntimeLogDebug({logging::Tag::kGCMark}, "Marked %p", object);
            }
            return pushed;
        }

        static PERFORMANCE_INLINE void processInMark(MarkQueue& markQueue, ObjHeader* object) noexcept {
            if (__builtin_expect(reinterpret_cast<uintptr_t>(object->typeInfoOrMeta_) == 0, false)) reportUnconstructed(object);
            auto process = object->type_info()->processObjectInMark;
            RuntimeAssert(process != nullptr, "Got null processObjectInMark for object %p", object);
            process(static_cast<void*>(&markQueue), object);
        }
    };

    class ThreadData : private Pinned {
        friend ConcurrentMark;

    public:
        ThreadData(ConcurrentMark& mark, mm::ThreadData& threadData) noexcept : threadData_(threadData) {}

        auto& markQueue() noexcept { return markQueue_; }
        void onSafePoint() noexcept;
        void onSuspendForGC() noexcept;

        bool tryLockRootSet() noexcept;
        void publish() noexcept;
        void clearMarkFlags() noexcept;

    private:
        void ensureFlushActionExecuted() noexcept;

        mm::ThreadData& threadData_;

        ManuallyScoped<MutatorQueue, true> markQueue_{};
        ManuallyScoped<OnceExecutable, true> flushAction_{};

        std::atomic<bool> rootSetLocked_ = false;
    };

    void setupBeforeSTW(GCHandle gcHandle);

    /** To be run by a single "main" GC thread during STW. */
    void markInSTW();

    // Kore: young collection, entirely in STW and on the GC thread. `markRememberedSets(worker, stackCandidates)` must
    // enqueue the young objects referenced from the old generation, and the heap objects the conservative stack
    // candidates point into (one walk of the heap pages for both).
    void markYoungInSTW(
            GCHandle gcHandle,
            const std::function<void(ParallelProcessor::Worker&, std::vector<ObjHeader**>&)>& markRememberedSets);

    ~ConcurrentMark();

    // Kore: runs `task` on the calling thread and on the GC helper threads; returns when all of them have finished.
    void runInParallel(const std::function<void()>& task) noexcept;

    // Kore: stops the young-collection mark helpers.
    void requestShutdown();

    /**
     * Weak reference reads may be mutually exclusive with certain parts of mark oprocess.
     * Every read must be guarded by the object returned by this method.
     */
    auto weakReadProtector() noexcept {
        auto markTerminationGuard = std::shared_lock{markTerminationMutex_, std::defer_lock};
        while (!markTerminationGuard.try_lock()) {
            mm::safePoint();
        }
        return markTerminationGuard;
    }

    /** Ensures that the mark phase would not be run during the execution of the guarded code. Use with care. */
    auto markMutex() noexcept {
        return std::shared_lock{markMutex_};
    }

private:
    GCHandle& gcHandle();

    void completeMutatorsRootSet(MarkTraits::MarkQueue& markQueue);
    void tryCollectRootSet(mm::ThreadData& thread, ParallelProcessor::Worker& markQueue);
    bool tryTerminateMark(std::size_t& everSharedBatches) noexcept;
    void flushMutatorQueues() noexcept;
    void endMarkingEpoch();

    void resetMutatorFlags();

    // Kore: GC helper threads (young collections' mark, sweeping), parked between uses.
    void startHelpers(std::function<void()> task) noexcept;
    void waitHelpers() noexcept;
    void helperBody() noexcept;

    std::vector<ScopedThread> helpers_;
    std::mutex helpersMutex_;
    std::condition_variable helpersStart_;
    std::condition_variable helpersDone_;
    uint64_t helpersRound_ = 0;
    size_t helpersRunning_ = 0;
    bool helpersShutdown_ = false;
    std::function<void()> helpersTask_;

    GCHandle gcHandle_ = GCHandle::invalid();
    std::optional<mm::ThreadRegistry::Iterable> lockedMutatorsList_;
    ManuallyScoped<ParallelProcessor, true> parallelProcessor_{};

    RWSpinLock markTerminationMutex_;
    RWSpinLock markMutex_;
};

namespace test_support {
bool flushActionRequested();
}

} // namespace kotlin::gc::mark
