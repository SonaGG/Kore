/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#include "GCImpl.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <cstdlib>

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
#include "ObjectTraversal.hpp"
#include "RootSet.hpp"
#include "ExternalRCRefRegistry.hpp"
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
    return object != nullptr && object->heap() && 
            (!alloc::objectDataForObject(object).oldOrSurviving() ||
             // Kore: nursery objects stay young through a full collection (whose mark does not make them old): stores
             // of them into old objects during its concurrent mark must be remembered for the next young collection.
             alloc::nursery::movable(object));
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
                configuredMiB > 0 ? static_cast<int64_t>(configuredMiB) * 1024 * 1024
                // Kore: with a nursery, collect before it fills up (allocation then goes to the regular heap).
                : compiler::gcNurserySize() > 0 ? static_cast<int64_t>(compiler::gcNurserySize()) * 1024 * 1024 / 4 * 3
                : kDefaultYoungGenerationBytes,
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

// ---------------------------------------------------------------------------------------------------------------------
// Kore: the moving young generation (`alloc::nursery`).
//
// A young collection marks as usual, after pinning the nursery objects referenced from roots: conservatively from the
// stacks (the stack memory of the suspended threads, which holds every reference the compiled code keeps across a GC
// point in its frame slots), and precisely from globals, thread-locals and external references. While marking, it
// records the heap slots that refer to movable nursery objects. Then it evacuates: every marked, unpinned nursery object
// gets a new place in the regular heap (its word forwards there), the recorded slots are updated, and the objects are
// copied. Pinned survivors stay, and their blocks join the old generation. Everything that survives becomes old.
// ---------------------------------------------------------------------------------------------------------------------

namespace {

std::atomic<bool> gRecordNurserySlots = false;
[[clang::no_destroy]] std::mutex gNurserySlotListsMutex;
[[clang::no_destroy]] std::vector<std::vector<ObjHeader**>*> gNurserySlotLists;
thread_local std::vector<ObjHeader**>* tNurserySlots = nullptr;

NO_INLINE std::vector<ObjHeader**>& nurserySlots() noexcept {
    if (tNurserySlots == nullptr) {
        tNurserySlots = new std::vector<ObjHeader**>();
        std::unique_lock guard(gNurserySlotListsMutex);
        gNurserySlotLists.push_back(tNurserySlots);
    }
    return *tNurserySlots;
}

ALWAYS_INLINE void recordIfMovable(ObjHeader** slot) noexcept {
    ObjHeader* value = *slot;
    if (value != nullptr && alloc::nursery::movable(value)) nurserySlots().push_back(slot);
}

ALWAYS_INLINE bool recordingNurserySlots() noexcept {
    return gRecordNurserySlots.load(std::memory_order_relaxed);
}

// During a full mark: heap objects outside of the nursery that refer to nursery objects. Nursery objects stay young
// through a full collection while the others become old, so these become old-to-young references, which the next
// young collection must know (like the ones the barriers remember).
std::atomic<bool> gRecordNurseryHolders = false;
[[clang::no_destroy]] std::vector<std::vector<ObjHeader*>*> gNurseryHolderLists;
thread_local std::vector<ObjHeader*>* tNurseryHolders = nullptr;

NO_INLINE std::vector<ObjHeader*>& nurseryHolders() noexcept {
    if (tNurseryHolders == nullptr) {
        tNurseryHolders = new std::vector<ObjHeader*>();
        std::unique_lock guard(gNurserySlotListsMutex);
        gNurseryHolderLists.push_back(tNurseryHolders);
    }
    return *tNurseryHolders;
}

ALWAYS_INLINE bool recordingNurseryHolders() noexcept {
    return gRecordNurseryHolders.load(std::memory_order_relaxed);
}

template <typename Traverse>
NO_INLINE void recordIfHolderOfNurseryObjects(ObjHeader* holder, Traverse traverse) noexcept {
    if (!holder->heap() || alloc::nursery::movable(holder)) return;
    bool refers = false;
    traverse([&](ObjHeader* value) { refers |= value != nullptr && alloc::nursery::movable(value); });
    if (refers) nurseryHolders().push_back(holder);
}

void pinIfMovable(ObjHeader* object) noexcept {
    if (object != nullptr && alloc::nursery::movable(object)) alloc::nursery::pin(object);
}

// The offset of the object header in its heap object (after the GC word).
ptrdiff_t objectOffset(ObjHeader* object) noexcept {
    return reinterpret_cast<uint8_t*>(object) - reinterpret_cast<uint8_t*>(&alloc::objectDataForObject(object));
}

} // namespace

namespace {

// Kore: debugging (`KORE_GC_VERIFY` set): after an evacuation, no reachable object may refer to a released nursery block.
void verifyAfterEvacuation() noexcept {
    std::vector<ObjHeader*> stack;
    std::unordered_set<ObjHeader*> visited;
    auto visit = [&](ObjHeader* object, const char* from, ObjHeader* holder) {
        if (object == nullptr || isNullOrMarker(object)) return;
        if (alloc::nursery::inFreeBlock(object)) {
            uintptr_t word = holder != nullptr && holder->heap() ? alloc::objectDataForObject(holder).word() : 0;
            uint32_t count = holder != nullptr && holder->type_info()->IsArray() ? holder->array()->count_ : 0;
            konan::consoleErrorf(
                    "kore-gc-verify: reference to a released nursery block %p from %s %p (type %p, word %p, array count %u, "
                    "retained %d, nursery %d)\n",
                    object, from, holder, holder != nullptr ? holder->type_info() : nullptr, reinterpret_cast<void*>(word), count,
                    holder != nullptr && alloc::nursery::inRetainedBlock(holder), holder != nullptr && alloc::nursery::movable(holder));
            std::abort();
        }
        if (!object->heap()) return;
        if (visited.insert(object).second) stack.push_back(object);
    };
    for (auto& thread : mm::ThreadRegistry::Instance().LockForIter()) {
        for (auto value : mm::ThreadRootSet(thread)) visit(value.object, "thread root", nullptr);
    }
    for (auto value : mm::GlobalRootSet()) visit(value.object, "global root", nullptr);
    while (!stack.empty()) {
        ObjHeader* object = stack.back();
        stack.pop_back();
        if (object->type_info() == nullptr) {
            konan::consoleErrorf("kore-gc-verify: object %p without type info\n", object);
            std::abort();
        }
        traverseReferredObjects(object, [&](ObjHeader* field) { visit(field, "object", object); });
    }
    konan::consoleErrorf("kore-gc-verify: %zu objects OK\n", visited.size());
}

} // namespace

namespace {

// Kore: debugging (`KORE_GC_VERIFY`): at the start of a young collection, every old object that refers to a movable
// nursery object must be remembered.
void verifyRememberedBeforeYoung(alloc::Allocator& allocator, uint64_t epoch) noexcept {
    size_t checked = 0;
    allocator.TraverseAllocatedObjects([&](ObjHeader* holder) {
        auto& data = alloc::objectDataForObject(holder);
        if (!data.old()) return;
        ++checked;
        traverseObjectFields(holder, [&](auto accessor) {
            ObjHeader* value = accessor.direct();
            if (value != nullptr && alloc::nursery::movable(value) && !data.remembered()) {
                konan::consoleErrorf(
                        "kore-gc-verify: epoch %llu: old %p (type %p, word %p, count %u) refers to nursery %p (word %p) unremembered\n",
                        static_cast<unsigned long long>(epoch), holder, holder->type_info(), reinterpret_cast<void*>(data.word()),
                        holder->type_info()->IsArray() ? holder->array()->count_ : 0u, value,
                        reinterpret_cast<void*>(alloc::objectDataForObject(value).word()));
                std::abort();
            }
        });
    });
    konan::consoleErrorf("kore-gc-verify: epoch %llu: %zu old objects checked\n", static_cast<unsigned long long>(epoch), checked);
}

} // namespace

void gc::internal::GenerationalGC::pinNurseryRoots(std::vector<ObjHeader*>& stackReferenced) noexcept {
    for (auto& thread : mm::ThreadRegistry::Instance().LockForIter()) {
        auto& suspension = thread.suspensionData();
        uintptr_t low = suspension.savedStackPointer() & ~uintptr_t(7);
        uintptr_t high = suspension.stackBase();
        for (uintptr_t address = low; address + sizeof(uintptr_t) <= high; address += sizeof(uintptr_t)) {
            uintptr_t word = *reinterpret_cast<const uintptr_t*>(address);
            ObjHeader* object = alloc::nursery::movableObjectContaining(word);
            if (object != nullptr && !alloc::nursery::pinned(object)) {
                alloc::nursery::pin(object);
                stackReferenced.push_back(object);
            }
        }
        for (size_t i = 0; i < mm::ThreadSuspensionData::kSavedRegisters; ++i) {
            ObjHeader* object = alloc::nursery::movableObjectContaining(suspension.savedRegisters()[i]);
            if (object != nullptr && !alloc::nursery::pinned(object)) {
                alloc::nursery::pin(object);
                stackReferenced.push_back(object);
            }
        }
        for (auto value : mm::ThreadRootSet(thread)) pinIfMovable(value.object);
    }
    for (auto value : mm::GlobalRootSet()) pinIfMovable(value.object);
    for (auto reference : mm::ExternalRCRefRegistry::instance().lockForIter()) {
        pinIfMovable(reference.load(std::memory_order_relaxed));
    }
}

void gc::internal::GenerationalGC::evacuateNursery() noexcept {
    struct Move {
        ObjHeader* from;
        ObjHeader* to;
        size_t bytes;
        ptrdiff_t offset;
    };
    std::vector<Move> moves;
    alloc::nursery::forEachMovableObject([&](ObjHeader* object) noexcept {
        auto& data = alloc::objectDataForObject(object);
        if (!data.marked()) return; // Dead.
        // Objects with extra object data (weak references, associated objects) are referred to from it: they stay.
        if (alloc::nursery::pinned(object) || object->has_meta_object()) {
            alloc::nursery::retain(object);
            return;
        }
        size_t bytes = alloc::allocatedHeapSize(object);
        ptrdiff_t offset = objectOffset(object);
        auto* to = reinterpret_cast<ObjHeader*>(allocator_.allocateForEvacuation(bytes) + offset);
        moves.push_back({object, to, bytes, offset});
        data.forwardTo(to);
    });
    // Update the slots that refer to moved objects (in place: slots inside moved objects are copied afterwards).
    std::vector<std::vector<ObjHeader**>*> slotLists;
    {
        std::unique_lock guard(gNurserySlotListsMutex);
        slotLists = gNurserySlotLists;
    }
    for (auto* slots : slotLists) {
        for (ObjHeader** slot : *slots) {
            ObjHeader* value = *slot;
            if (value == nullptr || !alloc::nursery::movable(value)) continue;
            auto& data = alloc::objectDataForObject(value);
            if (data.forwarded()) *slot = data.forwardee();
        }
        slots->clear();
    }
    for (const auto& move : moves) {
        std::memcpy(reinterpret_cast<uint8_t*>(move.to) - move.offset, reinterpret_cast<uint8_t*>(move.from) - move.offset, move.bytes);
        alloc::objectDataForObject(move.to).reset(GC::ObjectData::kOld);
    }
    // What stays in the nursery: survivors are promoted in place, the rest is dead.
    alloc::nursery::forEachMovableObject([](ObjHeader* object) noexcept {
        auto& data = alloc::objectDataForObject(object);
        data.reset(!data.forwarded() && data.marked() ? GC::ObjectData::kOld : 0);
    });
    allocator_.finishEvacuation();
    alloc::nursery::finishYoungCollection();
    static const bool verify = std::getenv("KORE_GC_VERIFY") != nullptr;
    if (verify) verifyAfterEvacuation();
    RuntimeLogDebug({kTagGC}, "Nursery: %zu objects moved", moves.size());
}

void gc::internal::GenerationalGC::finishNurseryAfterFullMark() noexcept {
    gRecordNurseryHolders.store(false, std::memory_order_relaxed);
    // Surviving holders of nursery objects become old-to-young references: remember them.
    {
        std::unique_lock guard(gNurserySlotListsMutex);
        std::unique_lock orphanedGuard(orphanedMutex_);
        for (auto* holders : gNurseryHolderLists) {
            for (ObjHeader* holder : *holders) {
                if (alloc::objectDataForObject(holder).tryRemember()) orphaned_.objects.push_back(holder);
            }
            holders->clear();
        }
    }
    // Nursery objects are swept by young collections, not by the allocator: reset their mark state. Retained blocks hold
    // old objects: the marked ones stay old, the others are dead.
    alloc::nursery::forEachMovableObject([](ObjHeader* object) noexcept { alloc::objectDataForObject(object).reset(0); });
    alloc::nursery::releaseEmptyRetainedBlocks([](ObjHeader* object) noexcept {
        auto& data = alloc::objectDataForObject(object);
        bool alive = (data.word() & ~GC::ObjectData::kFlags) != 0;
        data.reset(alive ? (data.word() & GC::ObjectData::kRemembered) | GC::ObjectData::kOld : 0);
        return alive;
    });
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
        if (recordingNurserySlots()) recordIfMovable(slot);
    });
    // Kore: slots in old objects of retained nursery blocks, which the allocator does not know.
    if (alloc::nursery::enabled()) {
        for (ObjHeader** slot : rememberedSet.slots) {
            if (!alloc::nursery::inRetainedBlock(slot)) continue;
            ObjHeader* owner = alloc::nursery::retainedObjectContaining(reinterpret_cast<uintptr_t>(slot));
            if (owner == nullptr || !alloc::objectDataForObject(owner).old()) continue;
            ObjHeader* value = std_support::atomic_ref{*slot}.load(std::memory_order_relaxed);
            if (value != nullptr && value->heap()) MarkTraits::tryEnqueue(markQueue, value);
            if (recordingNurserySlots()) recordIfMovable(slot);
        }
    }
}

void gc::internal::GenerationalGC::performYoungCollection(GCHandle gcHandle) noexcept {
    stopTheWorld(gcHandle, "GC stop the world: young collection");
    gcScheduler_.onGCStart();
    state_.start(gcHandle.getEpoch());

    // Kore: the moving young generation.
    bool nursery = alloc::nursery::enabled();
    std::vector<ObjHeader*> stackReferenced;
    if (nursery) {
        alloc::nursery::retireAll();
        static const bool verify = std::getenv("KORE_GC_VERIFY") != nullptr;
        if (verify) verifyRememberedBeforeYoung(allocator_, gcHandle.getEpoch());
        if (verify && alloc::nursery::activeBlocks() != 0) {
            konan::consoleErrorf("kore-gc-verify: %zu nursery blocks still active after retiring\n", alloc::nursery::activeBlocks());
            std::abort();
        }
        pinNurseryRoots(stackReferenced);
        gRecordNurserySlots.store(true, std::memory_order_relaxed);
    }

    RememberedSet rememberedSet;
    collectRememberedSets(rememberedSet);
    mark_.markYoungInSTW(gcHandle, [&](mark::ConcurrentMark::ParallelProcessor::Worker& worker) {
        markRememberedSet(worker, rememberedSet);
        // Conservatively found references only pin: liveness comes from the precise roots, as in full collections (a
        // live reference is always in a frame slot; other stack words may be stale).
    });

    if (nursery) {
        gRecordNurserySlots.store(false, std::memory_order_relaxed);
        evacuateNursery();
    }
}

void gc::internal::GenerationalGC::performFullCollection(GCHandle gcHandle) noexcept {
    // Kore: while marking concurrently, mutators allocate in the regular heap (allocated black).
    alloc::nursery::setAllocationEnabled(false);
    gRecordNurseryHolders.store(alloc::nursery::enabled(), std::memory_order_relaxed);
    mark_.setupBeforeSTW(gcHandle);
    stopTheWorld(gcHandle, "GC stop the world: full collection");
    alloc::nursery::retireAll();
    gcScheduler_.onGCStart();
    state_.start(gcHandle.getEpoch());

    // Marks concurrently with the mutators; returns with the world stopped again and the barriers disabled.
    mark_.markInSTW();

    keepLiveRememberedSetEntries();
    finishNurseryAfterFullMark();
    alloc::nursery::setAllocationEnabled(true);
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

namespace {

// Kore: debugging (`KORE_GC_CHECK`): every reference the marker follows must point to a constructed object.
NO_INLINE void checkFieldsInMark(ObjHeader* holder) noexcept {
    size_t index = 0;
    traverseObjectFields(holder, [&](auto accessor) noexcept {
        ObjHeader* value = accessor.direct();
        bool deadRetained = value != nullptr && alloc::nursery::inRetainedBlock(value) && alloc::objectDataForObject(value).word() == 0;
        if (value != nullptr && value->heap() && (reinterpret_cast<uintptr_t>(value->typeInfoOrMeta_) == 0 || deadRetained)) {
            uintptr_t word = holder->heap() ? alloc::objectDataForObject(holder).word() : 0;
            konan::consoleErrorf(
                    "kore-gc-check: %p (type %p size %d refs %d meta %d, word %p, nursery %d, retained %d) field #%zu -> %p "
                    "without type info or dead (dead retained %d, young %d, nursery %d, retained %d, free %d)\n",
                    holder, holder->type_info(), holder->type_info()->instanceSize_, holder->type_info()->objOffsetsCount_,
                    holder->has_meta_object(), reinterpret_cast<void*>(word), alloc::nursery::movable(holder),
                    alloc::nursery::inRetainedBlock(holder), index, value, deadRetained, gc::internal::isYoungCollection(),
                    alloc::nursery::movable(value), alloc::nursery::inRetainedBlock(value), alloc::nursery::inFreeBlock(value));
            std::abort();
        }
        ++index;
    });
}

} // namespace

// static
PERFORMANCE_INLINE void gc::GC::processObjectInMark(void* state, ObjHeader* object) noexcept {
    static const bool check = std::getenv("KORE_GC_CHECK") != nullptr;
    if (__builtin_expect(check, false)) checkFieldsInMark(object);
    if (__builtin_expect(recordingNurseryHolders(), false)) {
        recordIfHolderOfNurseryObjects(object, [object](auto check) {
            traverseClassObjectFields(object, [&check](auto accessor) noexcept { check(accessor.direct()); });
        });
    }
    if (__builtin_expect(recordingNurserySlots(), false)) {
        traverseClassObjectFields(object, [](auto accessor) noexcept { recordIfMovable(accessor.direct().location()); });
    }
    gc::internal::processObjectInMark<MarkTraits>(state, object);
}

// static
PERFORMANCE_INLINE void gc::GC::processArrayInMark(void* state, ArrayHeader* array) noexcept {
    static const bool check = std::getenv("KORE_GC_CHECK") != nullptr;
    if (__builtin_expect(check, false)) checkFieldsInMark(array->obj());
    if (__builtin_expect(recordingNurseryHolders(), false)) {
        recordIfHolderOfNurseryObjects(array->obj(), [array](auto check) {
            traverseArrayOfObjectsElements(array, [&check](auto accessor) noexcept { check(accessor.direct()); });
        });
    }
    if (__builtin_expect(recordingNurserySlots(), false)) {
        traverseArrayOfObjectsElements(array, [](auto accessor) noexcept { recordIfMovable(accessor.direct().location()); });
    }
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

// Kore: debugging the moving young generation.
void gc::mark::ConcurrentMark::MarkTraits::reportUnconstructed(ObjHeader* object) noexcept {
    konan::consoleErrorf(
            "kore-gc: marking %p without type info (young collection %d, nursery movable %d, retained %d, free %d)\n", object,
            internal::isYoungCollection(), alloc::nursery::movable(object), alloc::nursery::inRetainedBlock(object),
            alloc::nursery::inFreeBlock(object));
    std::abort();
}

bool gc::mark::ConcurrentMark::MarkTraits::checking() noexcept {
    static const bool check = std::getenv("KORE_GC_CHECK") != nullptr;
    return check;
}

void gc::mark::ConcurrentMark::MarkTraits::checkEnqueue(ObjHeader* object) noexcept {
    if (alloc::nursery::inRetainedBlock(object) && alloc::objectDataForObject(object).word() == 0) {
        konan::consoleErrorf("kore-gc-check: enqueueing dead object %p of a retained block (young %d)\n", object,
                             internal::isYoungCollection());
        std::abort();
    }
}
