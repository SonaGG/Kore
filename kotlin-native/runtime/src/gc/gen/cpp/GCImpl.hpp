/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <vector>

#include "Allocator.hpp"
#include "Barriers.hpp"
#include "ConcurrentMark.hpp"
#include "GC.hpp"
#include "GCScheduler.hpp"
#include "GCState.hpp"
#include "GCStatistics.hpp"
#include "Utils.hpp"
#include "concurrent/UtilityThread.hpp"

namespace kotlin::gc {

namespace internal {

// Old->young edges recorded by the write barriers of one mutator thread since the previous young collection.
struct RememberedSet {
    // Heap reference slots that received a young object (generic barrier: the owning object is unknown).
    std::vector<ObjHeader**> slots;
    // Old objects that received a young object (holder-aware barrier). Each is logged once, see `ObjectData::kRemembered`.
    std::vector<ObjHeader*> objects;

    void clear() noexcept {
        slots.clear();
        objects.clear();
    }

    void moveInto(RememberedSet& target) noexcept {
        target.slots.insert(target.slots.end(), slots.begin(), slots.end());
        target.objects.insert(target.objects.end(), objects.begin(), objects.end());
        clear();
    }
};

// Young collections: stop-the-world mark of the young generation, then a concurrent sweep.
// Full collections: concurrent mark of the whole heap (the `cms` collector's marker), then a concurrent sweep.
class GenerationalGC : private Pinned {
public:
    GenerationalGC(alloc::Allocator& allocator, gcScheduler::GCScheduler& gcScheduler, GCStateHolder& state, mark::ConcurrentMark& mark) noexcept;
    ~GenerationalGC();

    int64_t schedule(bool full) noexcept;

    // Called from the destructor of an exiting thread's data, with the thread registry locked.
    void adoptRememberedSet(RememberedSet& set) noexcept;

    // Test support.
    void clearForTests() noexcept;
    std::optional<bool> lastCollectionWasYoung() const noexcept;

private:
    void body() noexcept;
    void performCollection(int64_t epoch) noexcept;
    void performYoungCollection(GCHandle gcHandle) noexcept;
    void performFullCollection(GCHandle gcHandle) noexcept;
    size_t initialFullCollectionThresholdBytes() const noexcept;
    void collectRememberedSets(RememberedSet& into) noexcept;
    void keepLiveRememberedSetEntries() noexcept;
    void markRememberedSet(mark::ConcurrentMark::ParallelProcessor::Worker& markQueue, RememberedSet& rememberedSet) noexcept;

    alloc::Allocator& allocator_;
    gcScheduler::GCScheduler& gcScheduler_;
    GCStateHolder& state_;
    mark::ConcurrentMark& mark_;

    // Set by `schedule(true)`; consumed when a collection starts.
    std::atomic<bool> fullCollectionRequested_ = false;
    // Major collection policy: the heap after a young collection exceeded this many bytes.
    size_t fullCollectionThresholdBytes_ = 0;
    bool fullCollectionDue_ = false;
    std::atomic<int> lastCollectionKind_ = -1; // -1: none yet, 0: full, 1: young.

    std::mutex orphanedMutex_;
    RememberedSet orphaned_;

    UtilityThread thread_;
};

} // namespace internal

class GC::Impl : private Pinned {
public:
    Impl(alloc::Allocator& allocator, gcScheduler::GCScheduler& gcScheduler) noexcept :
        gc_(allocator, gcScheduler, state_, mark_) {}

    GCStateHolder state_;
    mark::ConcurrentMark mark_{};
    internal::GenerationalGC gc_;
};

class GC::ThreadData::Impl : private Pinned {
public:
    Impl(GC& gc, mm::ThreadData& threadData) noexcept : mark_(gc.impl().mark_, threadData), gc_(gc) {}
    ~Impl() { gc_.impl().gc_.adoptRememberedSet(rememberedSet_); }

    internal::RememberedSet& rememberedSet() noexcept { return rememberedSet_; }

    barriers::BarriersThreadData barriers_;
    mark::ConcurrentMark::ThreadData mark_;

private:
    GC& gc_;
    internal::RememberedSet rememberedSet_;
};

} // namespace kotlin::gc
