/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#include "GCImpl.hpp"

#include <algorithm>
#include <cinttypes>
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

namespace {

// Allocation volume between young collections when the scheduler does not configure one.
constexpr int64_t kDefaultYoungGenerationBytes = 16 * 1024 * 1024;

ALWAYS_INLINE gc::internal::RememberedSet& currentRememberedSet() noexcept {
    return mm::ThreadRegistry::Instance().CurrentThreadData()->gc().impl().rememberedSet();
}

NO_INLINE void rememberSlot(ObjHeader** slot) noexcept {
    currentRememberedSet().slots.push_back(slot);
}

NO_INLINE void rememberObject(ObjHeader* holder) noexcept {
    currentRememberedSet().objects.push_back(holder);
}

ALWAYS_INLINE bool isYoungHeapObject(ObjHeader* object) noexcept {
    return object != nullptr && object->heap() && !alloc::objectDataForObject(object).marked();
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// GenerationalGC
// ---------------------------------------------------------------------------------------------------------------------

gc::internal::GenerationalGC::GenerationalGC(alloc::Allocator& allocator, gcScheduler::GCScheduler& gcScheduler) noexcept :
    allocator_(allocator),
    gcScheduler_(gcScheduler),
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

size_t gc::internal::GenerationalGC::initialFullCollectionThresholdBytes() const noexcept {
    // Until the first full collection measured the live heap, use the scheduler's initial heap target.
    auto& config = gcScheduler_.config();
    return static_cast<size_t>(std::max(config.targetHeapBytes.load(std::memory_order_relaxed), config.minHeapBytes.load(std::memory_order_relaxed)));
}

gc::internal::GenerationalGC::~GenerationalGC() {
    state_.shutdown();
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

void gc::internal::GenerationalGC::dropRememberedSets() noexcept {
    RememberedSet dropped;
    collectRememberedSets(dropped);
}

void gc::internal::GenerationalGC::clearMarks() noexcept {
    allocator_.TraverseAllocatedObjects([](ObjHeader* object) noexcept { alloc::objectDataForObject(object).reset(); });
}

void gc::internal::GenerationalGC::markRememberedSet(MarkQueue& markQueue, RememberedSet& rememberedSet) noexcept {
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
    // Slots recorded by the generic barrier: only those inside old (marked) objects are roots. Slots in young objects
    // are traced if their object is reachable; stack, global and thread-local slots are part of the root set already.
    allocator_.forEachHeapSlot(rememberedSet.slots, [&markQueue](ObjHeader** slot, ObjHeader* owner) noexcept {
        if (!alloc::objectDataForObject(owner).marked()) return;
        ObjHeader* value = std_support::atomic_ref{*slot}.load(std::memory_order_relaxed);
        if (value != nullptr && value->heap()) {
            MarkTraits::tryEnqueue(markQueue, value);
        }
    });
}

void gc::internal::GenerationalGC::performCollection(int64_t epoch) noexcept {
    auto mainGCLock = mm::GlobalData::Instance().gc().gcLock();

    bool full = fullCollectionRequested_.exchange(false, std::memory_order_acq_rel) || fullCollectionDue_;
    auto gcHandle = GCHandle::create(epoch);
    if (!full) gcHandle.youngCollection();

    stopTheWorld(gcHandle, full ? "GC stop the world: full collection" : "GC stop the world: young collection");

    gcScheduler_.onGCStart();

    state_.start(epoch);

    RememberedSet rememberedSet;
    if (full) {
        // Every object becomes young again; the full mark below re-marks (promotes) everything that is alive.
        dropRememberedSets();
        clearMarks();
    } else {
        collectRememberedSets(rememberedSet);
    }

    collectRootSet<MarkTraits>(gcHandle, markQueue_, [](mm::ThreadData&) { return true; });
    if (!full) {
        markRememberedSet(markQueue_, rememberedSet);
    }
    Mark<MarkTraits>(gcHandle, markQueue_);
    processWeaks<DefaultProcessWeaksTraits>(gcHandle, mm::ExternalRCRefRegistry::instance());

    for (auto& thread : mm::ThreadRegistry::Instance().LockForIter()) {
        thread.allocator().prepareForGC();
    }
    if (full) {
        allocator_.prepareForGC();
    } else {
        allocator_.prepareForYoungGC();
    }

    allocator_.sweep(gcHandle);

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

    resumeTheWorld(gcHandle);

    state_.finish(epoch);
    gcHandle.finished();

    // This may start a new thread. On some pthreads implementations, this may block waiting for concurrent thread
    // destructors running. So, it must ensured that no locks are held by this point.
    allocator_.scheduleFinalization(gcHandle);
}

// ---------------------------------------------------------------------------------------------------------------------
// GC interface
// ---------------------------------------------------------------------------------------------------------------------

gc::GC::ThreadData::ThreadData(GC& gc, mm::ThreadData& threadData) noexcept : impl_(std::make_unique<Impl>(gc)) {}

gc::GC::ThreadData::~ThreadData() = default;

void gc::GC::ThreadData::OnSuspendForGC() noexcept {}

void gc::GC::ThreadData::safePoint() noexcept {}

void gc::GC::ThreadData::onThreadRegistration() noexcept {}

ALWAYS_INLINE void gc::GC::ThreadData::onAllocation(ObjHeader* object) noexcept {}

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
    gc::internal::processObjectInMark<gc::internal::MarkTraits>(state, object);
}

// static
PERFORMANCE_INLINE void gc::GC::processArrayInMark(void* state, ArrayHeader* array) noexcept {
    gc::internal::processArrayInMark<gc::internal::MarkTraits>(state, array);
}

int64_t gc::GC::Schedule() noexcept {
    return impl_->gc_.schedule(/* full = */ true);
}

int64_t gc::GC::ScheduleYoung() noexcept {
    return impl_->gc_.schedule(/* full = */ false);
}

void gc::GC::WaitFinished(int64_t epoch) noexcept {
    impl_->gc_.state().waitEpochFinished(epoch);
}

void gc::GC::WaitFinalizers(int64_t epoch) noexcept {
    impl_->gc_.state().waitEpochFinalized(epoch);
}

void gc::GC::onEpochFinalized(int64_t epoch) noexcept {
    GCHandle::getByEpoch(epoch).finalizersDone();
    impl_->gc_.state().finalized(epoch);
}

// Generic write barrier: every heap reference store from compiled code and the runtime ends up here. The owner of the
// slot is unknown (it can even be a stack object, a global or a thread-local), so record the slot itself whenever a
// young object is stored; the next young collection resolves it.
PERFORMANCE_INLINE void gc::beforeHeapRefUpdate(mm::DirectRefAccessor ref, ObjHeader* value, bool loadAtomic) noexcept {
    if (__builtin_expect(isYoungHeapObject(value), false)) {
        rememberSlot(ref.location());
    }
}

// Holder-aware barrier: log an old holder (once) when it receives a young object.
PERFORMANCE_INLINE void gc::afterReferenceStoreInObject(ObjHeader* holder, ObjHeader* value) noexcept {
    if (holder == nullptr || !holder->heap()) return;
    auto& holderData = alloc::objectDataForObject(holder);
    if (!holderData.marked()) return; // Young holders are traced anyway.
    if (!isYoungHeapObject(value)) return;
    if (holderData.tryRemember()) {
        rememberObject(holder);
    }
}

ALWAYS_INLINE OBJ_GETTER(gc::weakRefReadBarrier, std_support::atomic_ref<ObjHeader*> weakReferee) noexcept {
    RETURN_OBJ(weakReferee.load(std::memory_order_relaxed));
}

bool gc::isMarked(ObjHeader* object) noexcept {
    return alloc::objectDataForObject(object).marked();
}

// Sticky marks: survivors keep their mark (and become old). Only a full collection clears marks, before marking.
PERFORMANCE_INLINE bool gc::tryResetMark(GC::ObjectData& objectData) noexcept {
    return objectData.marked();
}

ALWAYS_INLINE bool gc::barriers::ExternalRCRefReleaseGuard::isNoop() {
    return true;
}
ALWAYS_INLINE gc::barriers::ExternalRCRefReleaseGuard::ExternalRCRefReleaseGuard(mm::DirectRefAccessor) noexcept {}
ALWAYS_INLINE gc::barriers::ExternalRCRefReleaseGuard::ExternalRCRefReleaseGuard(ExternalRCRefReleaseGuard&&) noexcept = default;
ALWAYS_INLINE gc::barriers::ExternalRCRefReleaseGuard::~ExternalRCRefReleaseGuard() noexcept = default;
ALWAYS_INLINE gc::barriers::ExternalRCRefReleaseGuard& gc::barriers::ExternalRCRefReleaseGuard::ExternalRCRefReleaseGuard::operator=(
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
