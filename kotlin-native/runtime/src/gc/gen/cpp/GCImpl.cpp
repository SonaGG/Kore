/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#include "GCImpl.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <memory>

#include "Allocator.hpp"
#include "CompilerConstants.hpp"
#include "ExternalRCRef.hpp"
#include "ExtraObjectData.hpp"
#include "GC.hpp"
#include "GCScheduler.hpp"
#include "GCStatistics.hpp"
#include "GlobalData.hpp"
#include "Logging.hpp"
#include "MarkAndSweepUtils.hpp"
#include "ObjectOps.hpp"
#include "ThreadData.hpp"
#include "ThreadRegistry.hpp"

using namespace kotlin;

std::atomic<bool> gc::internal::youngCollection = false;

namespace {

using MarkTraits = gc::mark::ConcurrentMark::MarkTraits;

// Allocation volume between young collections when `gcYoungGenerationSize` is not set.
constexpr int64_t kDefaultYoungGenerationBytes = 64 * 1024 * 1024;

ALWAYS_INLINE gc::internal::RememberedSet& currentRememberedSet() noexcept {
    return mm::ThreadRegistry::Instance().CurrentThreadData()->gc().impl().rememberedSet();
}

NO_INLINE void rememberSlot(ObjHeader** slot) noexcept {
    currentRememberedSet().slots.push_back(slot);
}

NO_INLINE void rememberObject(ObjHeader* holder) noexcept {
    currentRememberedSet().objects.push_back(holder);
}

// Allocated since the previous collection: neither old nor surviving the collection in progress. Objects allocated
// during a concurrent full mark are marked ("black"), so they don't count as young.
ALWAYS_INLINE bool isYoungHeapObject(ObjHeader* object) noexcept {
    return object != nullptr && object->heap() && !alloc::objectDataForObject(object).oldOrSurviving();
}

ALWAYS_INLINE void rememberIfOldToYoung(ObjHeader* holder, ObjHeader* value) noexcept {
    if (__builtin_expect(!holder->heap(), false)) return; // Stack objects are roots; permanent objects are immutable.
    auto& holderData = alloc::objectDataForObject(holder);
    if (__builtin_expect(!holderData.oldOrSurviving(), true)) return; // Young holders are traced anyway.
    if (!isYoungHeapObject(value)) return;
    if (holderData.tryRemember()) {
        rememberObject(holder);
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// GenerationalGC
// ---------------------------------------------------------------------------------------------------------------------

gc::internal::GenerationalGC::GenerationalGC(
        alloc::Allocator& allocator, gcScheduler::GCScheduler& gcScheduler, GCStateHolder& state, mark::ConcurrentMark& mark) noexcept :
    allocator_(allocator),
    gcScheduler_(gcScheduler),
    state_(state),
    mark_(mark),
    thread_(std::string_view("Main GC thread"), [this] { body(); }) {
    auto& youngGenerationBytes = gcScheduler_.config().youngGenerationBytes;
    if (youngGenerationBytes.load(std::memory_order_relaxed) == 0) {
        auto configuredMiB = compiler::gcYoungGenerationSize();
        youngGenerationBytes.store(
                configuredMiB > 0 ? static_cast<int64_t>(configuredMiB) * 1024 * 1024 : kDefaultYoungGenerationBytes,
                std::memory_order_relaxed);
    }
    fullCollectionThresholdBytes_ = initialFullCollectionThresholdBytes();
}

gc::internal::GenerationalGC::~GenerationalGC() {
    state_.shutdown();
}

size_t gc::internal::GenerationalGC::initialFullCollectionThresholdBytes() const noexcept {
    // Until the first full collection measured the live heap, use the scheduler's initial heap target.
    auto& config = gcScheduler_.config();
    return static_cast<size_t>(std::max(config.targetHeapBytes.load(std::memory_order_relaxed), config.minHeapBytes.load(std::memory_order_relaxed)));
}

int64_t gc::internal::GenerationalGC::schedule(bool full) noexcept {
    if (full) {
        // Must be set before scheduling: a collection that starts after this point will see it.
        fullCollectionRequested_.store(true, std::memory_order_release);
    }
    return state_.schedule();
}

void gc::internal::GenerationalGC::adoptRememberedSet(RememberedSet& set) noexcept {
    std::unique_lock guard(orphanedMutex_);
    set.moveInto(orphaned_);
}

void gc::internal::GenerationalGC::clearForTests() noexcept {
    std::unique_lock guard(orphanedMutex_);
    orphaned_.clear();
    fullCollectionRequested_.store(false, std::memory_order_relaxed);
    fullCollectionDue_ = false;
    fullCollectionThresholdBytes_ = initialFullCollectionThresholdBytes();
    lastCollectionKind_.store(-1, std::memory_order_relaxed);
    youngCollection.store(false, std::memory_order_relaxed);
}

std::optional<bool> gc::internal::GenerationalGC::lastCollectionWasYoung() const noexcept {
    switch (lastCollectionKind_.load(std::memory_order_acquire)) {
        case 0:
            return false;
        case 1:
            return true;
        default:
            return std::nullopt;
    }
}

void gc::internal::GenerationalGC::body() noexcept {
    RuntimeLogInfo({kTagGC}, "Initializing generational mark & sweep GC.");
    while (true) {
        if (auto epoch = state_.waitScheduled()) {
            performCollection(*epoch);
        } else {
            break;
        }
    }
    mark_.requestShutdown();
}

void gc::internal::GenerationalGC::collectRememberedSets(RememberedSet& into) noexcept {
    // Registry lock held: exiting threads hand their sets to `orphaned_` while holding the same lock, so every
    // recorded edge is either in a registered thread's set or in `orphaned_`. Threads that exit later cannot have
    // recorded anything new: barriers only run in the runnable state and the world is stopped.
    auto threads = mm::ThreadRegistry::Instance().LockForIter();
    for (auto& thread : threads) {
        thread.gc().impl().rememberedSet().moveInto(into);
    }
    std::unique_lock guard(orphanedMutex_);
    orphaned_.moveInto(into);
}

void gc::internal::GenerationalGC::keepLiveRememberedSetEntries() noexcept {
    // A full collection does not need the remembered sets, but the next young collection does: entries recorded
    // before or during this collection are kept for it. Logged objects that this collection found dead are dropped
    // (they are about to be freed). Slots are kept as they are: resolving a slot only trusts the allocator's current
    // pages and objects, and no object allocated after this collection can be old by the next young collection.
    RememberedSet all;
    collectRememberedSets(all);
    all.objects.erase(
            std::remove_if(all.objects.begin(), all.objects.end(), [](ObjHeader* holder) {
                return !alloc::objectDataForObject(holder).marked();
            }),
            all.objects.end());
    std::unique_lock guard(orphanedMutex_);
    all.moveInto(orphaned_);
}

void gc::internal::GenerationalGC::markRememberedSet(
        mark::ConcurrentMark::ParallelProcessor::Worker& markQueue, RememberedSet& rememberedSet) noexcept {
    // Objects logged by the holder-aware barrier: rescan all their references.
    for (ObjHeader* holder : rememberedSet.objects) {
        alloc::objectDataForObject(holder).forget();
        MarkTraits::processInMark(markQueue, holder);
        if (auto* extraObjectData = mm::ExtraObjectData::Get(holder)) {
            if (auto* weakReference = extraObjectData->GetRegularWeakReferenceImpl()) {
                if (weakReference->heap()) {
                    MarkTraits::tryEnqueue(markQueue, weakReference);
                }
            }
        }
    }
    // Slots recorded by the generic barrier: only those inside old objects are roots. Slots in young objects are
    // traced if their object is reachable; stack, global and thread-local slots are part of the root set already.
    allocator_.forEachHeapSlot(rememberedSet.slots, [&markQueue](ObjHeader** slot, ObjHeader* owner) noexcept {
        if (!alloc::objectDataForObject(owner).old()) return;
        ObjHeader* value = std_support::atomic_ref{*slot}.load(std::memory_order_relaxed);
        if (value != nullptr && value->heap()) {
            MarkTraits::tryEnqueue(markQueue, value);
        }
    });
}

void gc::internal::GenerationalGC::performYoungCollection(GCHandle gcHandle) noexcept {
    stopTheWorld(gcHandle, "GC stop the world: young collection");
    gcScheduler_.onGCStart();
    state_.start(gcHandle.getEpoch());

    RememberedSet rememberedSet;
    collectRememberedSets(rememberedSet);
    mark_.markYoungInSTW(gcHandle, [&](mark::ConcurrentMark::ParallelProcessor::Worker& worker) {
        markRememberedSet(worker, rememberedSet);
    });
}

void gc::internal::GenerationalGC::performFullCollection(GCHandle gcHandle) noexcept {
    mark_.setupBeforeSTW(gcHandle);
    stopTheWorld(gcHandle, "GC stop the world: full collection");
    gcScheduler_.onGCStart();
    state_.start(gcHandle.getEpoch());

    // Marks concurrently with the mutators; returns with the world stopped again and the barriers disabled.
    mark_.markInSTW();

    keepLiveRememberedSetEntries();
}

void gc::internal::GenerationalGC::performCollection(int64_t epoch) noexcept {
    auto mainGCLock = mm::GlobalData::Instance().gc().gcLock();

    bool full = fullCollectionRequested_.exchange(false, std::memory_order_acq_rel) || fullCollectionDue_;
    youngCollection.store(!full, std::memory_order_relaxed);
    auto gcHandle = GCHandle::create(epoch);
    if (full) {
        performFullCollection(gcHandle);
    } else {
        gcHandle.youngCollection();
        performYoungCollection(gcHandle);
    }

    // The world is stopped and marking is complete.
    for (auto& thread : mm::ThreadRegistry::Instance().LockForIter()) {
        thread.allocator().prepareForGC();
    }
    if (full) {
        allocator_.prepareForGC();
    } else {
        allocator_.prepareForYoungGC();
    }
    resumeTheWorld(gcHandle);

    // Concurrent sweep (with the GC helper threads): the sooner it is done, the sooner allocation reuses swept pages
    // instead of fresh memory.
    allocator_.sweep(gcHandle, [this](const std::function<void()>& task) { mark_.runInParallel(task); });

    // Objects kept by this collection, including old objects in pages a young collection did not sweep.
    auto heapBytes = allocator_.objectBytes();
    auto& config = gcScheduler_.config();
    if (full) {
        double threshold = static_cast<double>(heapBytes) / config.targetHeapUtilization.load(std::memory_order_relaxed);
        fullCollectionThresholdBytes_ = std::max(static_cast<size_t>(threshold), static_cast<size_t>(config.minHeapBytes.load(std::memory_order_relaxed)));
        fullCollectionDue_ = false;
    } else {
        fullCollectionDue_ = heapBytes > fullCollectionThresholdBytes_;
    }
    RuntimeLogInfo(
            {kTagGC}, "Epoch #%" PRId64 ": %s collection, %zu bytes kept, next full collection above %zu bytes%s", epoch,
            full ? "full" : "young", heapBytes, fullCollectionThresholdBytes_, fullCollectionDue_ ? " (due)" : "");
    lastCollectionKind_.store(full ? 0 : 1, std::memory_order_release);

    gcScheduler_.onGCFinish(epoch, heapBytes);

    state_.finish(epoch);
    gcHandle.finished();

    // This may start a new thread. On some pthreads implementations, this may block waiting for concurrent thread
    // destructors running. So, it must ensured that no locks are held by this point.
    allocator_.scheduleFinalization(gcHandle);
}

// ---------------------------------------------------------------------------------------------------------------------
// GC interface
// ---------------------------------------------------------------------------------------------------------------------

gc::GC::ThreadData::ThreadData(GC& gc, mm::ThreadData& threadData) noexcept : impl_(std::make_unique<Impl>(gc, threadData)) {}

gc::GC::ThreadData::~ThreadData() = default;

void gc::GC::ThreadData::OnSuspendForGC() noexcept {
    impl_->mark_.onSuspendForGC();
}

void gc::GC::ThreadData::safePoint() noexcept {
    impl_->mark_.onSafePoint();
}

void gc::GC::ThreadData::onThreadRegistration() noexcept {
    impl_->barriers_.onThreadRegistration();
}

PERFORMANCE_INLINE void gc::GC::ThreadData::onAllocation(ObjHeader* object) noexcept {
    impl_->barriers_.onAllocation(object);
}

gc::GC::GC(alloc::Allocator& allocator, gcScheduler::GCScheduler& gcScheduler) noexcept :
    impl_(std::make_unique<Impl>(allocator, gcScheduler)) {
    RuntimeLogDebug({kTagGC}, "Generational mark & sweep GC initialized");
}

gc::GC::~GC() = default;

void gc::GC::ClearForTests() noexcept {
    GCHandle::ClearForTests();
    impl_->gc_.clearForTests();
}

// static
PERFORMANCE_INLINE void gc::GC::processObjectInMark(void* state, ObjHeader* object) noexcept {
    gc::internal::processObjectInMark<MarkTraits>(state, object);
}

// static
PERFORMANCE_INLINE void gc::GC::processArrayInMark(void* state, ArrayHeader* array) noexcept {
    gc::internal::processArrayInMark<MarkTraits>(state, array);
}

int64_t gc::GC::Schedule() noexcept {
    return impl_->gc_.schedule(/* full = */ true);
}

int64_t gc::GC::ScheduleYoung() noexcept {
    return impl_->gc_.schedule(/* full = */ false);
}

void gc::GC::WaitFinished(int64_t epoch) noexcept {
    impl_->state_.waitEpochFinished(epoch);
}

void gc::GC::WaitFinalizers(int64_t epoch) noexcept {
    impl_->state_.waitEpochFinalized(epoch);
}

void gc::GC::onEpochFinalized(int64_t epoch) noexcept {
    GCHandle::getByEpoch(epoch).finalizersDone();
    impl_->state_.finalized(epoch);
}

// Generic write barrier: stores without a known holder (volatile and atomic fields, atomic array elements, globals,
// thread-locals). Snapshot-at-the-beginning during a concurrent full mark; otherwise record the slot whenever a young
// object is stored, for the next young collection to resolve.
PERFORMANCE_INLINE void gc::beforeHeapRefUpdate(mm::DirectRefAccessor ref, ObjHeader* value, bool loadAtomic) noexcept {
    barriers::beforeHeapRefUpdate(ref, value, loadAtomic);
    if (__builtin_expect(isYoungHeapObject(value), false)) {
        rememberSlot(ref.location());
    }
}

// Write barrier for field and array element stores (`UpdateHeapRefInObject`), i.e. almost every heap reference store.
// Stores into young objects (the common case, e.g. initializing a new object) need nothing: young objects are traced
// if reachable.
PERFORMANCE_INLINE void gc::beforeHeapRefUpdateInObject(ObjHeader* holder, mm::DirectRefAccessor ref, ObjHeader* value) noexcept {
    barriers::beforeHeapRefUpdate(ref, value, false);
    rememberIfOldToYoung(holder, value);
}

// Copying references between arrays (e.g. growing an ArrayList): outside of a concurrent mark only old->young edges need
// recording, and one remembered holder covers all of them.
void gc::copyHeapRefsInObject(ObjHeader* holder, ObjHeader** destination, ObjHeader* const* source, size_t count) noexcept {
    if (__builtin_expect(barriers::markClosureBarriersEnabled(), false)) {
        if (destination <= source) {
            for (size_t i = 0; i < count; ++i) {
                mm::DirectRefAccessor ref{destination + i};
                beforeHeapRefUpdateInObject(holder, ref, source[i]);
                ref.store(source[i]);
            }
        } else {
            for (size_t i = count; i > 0; --i) {
                mm::DirectRefAccessor ref{destination + i - 1};
                beforeHeapRefUpdateInObject(holder, ref, source[i - 1]);
                ref.store(source[i - 1]);
            }
        }
        return;
    }
    std::memmove(destination, source, count * sizeof(ObjHeader*));
    if (!holder->heap() || !alloc::objectDataForObject(holder).oldOrSurviving()) return;
    for (size_t i = 0; i < count; ++i) {
        if (isYoungHeapObject(destination[i])) {
            rememberIfOldToYoung(holder, destination[i]);
            return;
        }
    }
}

// Stores that bypass the store functions (attaching a weak reference implementation to ExtraObjectData).
PERFORMANCE_INLINE void gc::afterReferenceStoreInObject(ObjHeader* holder, ObjHeader* value) noexcept {
    if (holder == nullptr || value == nullptr) return;
    rememberIfOldToYoung(holder, value);
}

PERFORMANCE_INLINE OBJ_GETTER(gc::weakRefReadBarrier, std_support::atomic_ref<ObjHeader*> weakReferee) noexcept {
    RETURN_OBJ(gc::barriers::weakRefReadBarrier(weakReferee));
}

// Alive in the collection in progress: marked, or old during a young collection (which does not mark old objects).
PERFORMANCE_INLINE bool gc::isMarked(ObjHeader* object) noexcept {
    auto& objectData = alloc::objectDataForObject(object);
    return objectData.marked() || (internal::isYoungCollection() && objectData.old());
}

// Sweep: survivors become old and their marks are reset. A young collection keeps unmarked old objects.
PERFORMANCE_INLINE bool gc::tryResetMark(GC::ObjectData& objectData) noexcept {
    return objectData.sweep(/* fullCollection = */ !internal::isYoungCollection());
}

ALWAYS_INLINE bool gc::barriers::ExternalRCRefReleaseGuard::isNoop() {
    return false;
}
PERFORMANCE_INLINE gc::barriers::ExternalRCRefReleaseGuard::ExternalRCRefReleaseGuard(mm::DirectRefAccessor ref) noexcept : impl_(ref) {}
PERFORMANCE_INLINE gc::barriers::ExternalRCRefReleaseGuard::ExternalRCRefReleaseGuard(ExternalRCRefReleaseGuard&& other) noexcept = default;
PERFORMANCE_INLINE gc::barriers::ExternalRCRefReleaseGuard::~ExternalRCRefReleaseGuard() noexcept = default;
PERFORMANCE_INLINE gc::barriers::ExternalRCRefReleaseGuard& gc::barriers::ExternalRCRefReleaseGuard::ExternalRCRefReleaseGuard::operator=(
        ExternalRCRefReleaseGuard&&) noexcept = default;

// static
ALWAYS_INLINE uint64_t type_layout::descriptor<gc::GC::ObjectData>::type::size() noexcept {
    return sizeof(gc::GC::ObjectData);
}

// static
ALWAYS_INLINE size_t type_layout::descriptor<gc::GC::ObjectData>::type::alignment() noexcept {
    return alignof(gc::GC::ObjectData);
}

// static
ALWAYS_INLINE gc::GC::ObjectData* type_layout::descriptor<gc::GC::ObjectData>::type::construct(uint8_t* ptr) noexcept {
    return new (ptr) gc::GC::ObjectData();
}
