/*
 * Copyright 2010-2023 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#include "AllocatorImpl.hpp"

#include <algorithm>

#include "Allocator.hpp"
#include "GCApi.hpp"
#include "Heap.hpp"
#include "ThreadData.hpp"

using namespace kotlin;

alloc::Allocator::ThreadData::ThreadData(Allocator& allocator) noexcept : impl_(std::make_unique<Impl>(allocator.impl())) {}

alloc::Allocator::ThreadData::~ThreadData() {
    // Kore: hand the nursery block back (under the GC lock: the thread is native, a collection may be running).
    if (Nursery::instance() != nullptr) {
        auto lock = mm::GlobalData::Instance().gc().gcLock();
        impl_->alloc().RetireNursery();
    }
}

PERFORMANCE_INLINE ObjHeader* alloc::Allocator::ThreadData::allocateObject(const TypeInfo* typeInfo) noexcept {
    return impl_->alloc().CreateObject(typeInfo);
}

PERFORMANCE_INLINE ArrayHeader* alloc::Allocator::ThreadData::allocateArray(const TypeInfo* typeInfo, uint32_t elements) noexcept {
    return impl_->alloc().CreateArray(typeInfo, elements);
}

PERFORMANCE_INLINE mm::ExtraObjectData& alloc::Allocator::ThreadData::allocateExtraObjectData(
        ObjHeader* object, const TypeInfo* typeInfo) noexcept {
    return *impl_->alloc().CreateExtraObjectDataForObject(object, typeInfo);
}

ALWAYS_INLINE void alloc::Allocator::ThreadData::destroyUnattachedExtraObjectData(mm::ExtraObjectData& extraObject) noexcept {
    extraObject.setFlag(mm::ExtraObjectData::FLAGS_SWEEPABLE);
}

void alloc::Allocator::ThreadData::prepareForGC() noexcept {
    impl_->alloc().PrepareForGC();
}

void alloc::Allocator::ThreadData::clearForTests() noexcept {
    impl_->alloc().PrepareForGC();
}

alloc::Allocator::Allocator() noexcept : impl_(std::make_unique<Impl>()) {}

alloc::Allocator::~Allocator() = default;

void alloc::Allocator::prepareForGC() noexcept {
    impl_->heap().PrepareForGC();
}

size_t alloc::Allocator::objectBytes() noexcept {
    return impl_->heap().allocatedSizeTracker().allocatedBytes();
}

void alloc::Allocator::prepareForYoungGC() noexcept {
    impl_->heap().PrepareForYoungGC();
}

void alloc::Allocator::forEachHeapSlot(
        std::vector<ObjHeader**>& slots, const std::function<void(ObjHeader**, ObjHeader*)>& process) noexcept {
    std::sort(slots.begin(), slots.end());
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
    impl_->heap().ForEachHeapSlot(slots, process);
}

void alloc::Allocator::clearForTests() noexcept {
    stopFinalizerThreadIfRunning();
    impl_->heap().ClearForTests();
    impl_->pendingFinalizers() = FinalizerQueue();
}

void alloc::Allocator::TraverseAllocatedObjects(std::function<void(ObjHeader*)> fn) noexcept {
    impl_->heap().TraverseAllocatedObjects(fn);
}

void alloc::Allocator::TraverseAllocatedExtraObjects(std::function<void(mm::ExtraObjectData*)> fn) noexcept {
    impl_->heap().TraverseAllocatedExtraObjects(fn);
}

void alloc::Allocator::startFinalizerThreadIfNeeded() noexcept {
    NativeOrUnregisteredThreadGuard guard(true);
    impl_->finalizerProcessor().startThreadIfNeeded();
}

void alloc::Allocator::stopFinalizerThreadIfRunning() noexcept {
    NativeOrUnregisteredThreadGuard guard(true);
    impl_->finalizerProcessor().stopThread();
}

bool alloc::Allocator::finalizersThreadIsRunning() noexcept {
    return impl_->finalizerProcessor().isThreadRunning();
}

void alloc::Allocator::configureMainThreadFinalizerProcessor(std::function<void(alloc::RunLoopFinalizerProcessorConfig&)> f) noexcept {
    impl_->finalizerProcessor().configureMainThread(std::move(f));
}

bool alloc::Allocator::mainThreadFinalizerProcessorAvailable() noexcept {
    return impl_->finalizerProcessor().mainThreadAvailable();
}

void alloc::Allocator::sweep(gc::GCHandle gcHandle) noexcept {
    sweep(gcHandle, [](const std::function<void()>& task) { task(); });
}

void alloc::Allocator::sweep(gc::GCHandle gcHandle, const std::function<void(const std::function<void()>&)>& runInParallel) noexcept {
    // also sweeps extraObjects
    auto finalizerQueue = impl_->heap().Sweep(gcHandle, runInParallel);
    for (auto& thread : kotlin::mm::ThreadRegistry::Instance().LockForIter()) {
        finalizerQueue.mergeFrom(thread.allocator().impl().alloc().ExtractFinalizerQueue());
    }
    finalizerQueue.mergeFrom(impl_->heap().ExtractFinalizerQueue());
    RuntimeAssert(impl_->pendingFinalizers().size() == 0, "pendingFinalizers_ were not empty");
    impl_->pendingFinalizers() = std::move(finalizerQueue);
}

void alloc::Allocator::scheduleFinalization(gc::GCHandle gcHandle) noexcept {
    auto queue = std::move(impl_->pendingFinalizers());
    gcHandle.finalizersScheduled(queue.size());
    impl_->finalizerProcessor().schedule(std::move(queue), gcHandle.getEpoch());
}

void alloc::initObjectPool() noexcept {}

void alloc::compactObjectPoolInCurrentThread() noexcept {}

gc::GC::ObjectData& alloc::objectDataForObject(ObjHeader* object) noexcept {
    return CustomHeapObject::from(object).heapHeader();
}

ObjHeader* alloc::objectForObjectData(gc::GC::ObjectData& objectData) noexcept {
    return CustomHeapObject::from(objectData).object();
}

size_t alloc::allocatedHeapSize(ObjHeader* object) noexcept {
    return CustomAllocator::GetAllocatedHeapSize(object);
}

size_t alloc::allocatedBytes() noexcept {
    return GetAllocatedBytes();
}

void alloc::destroyExtraObjectData(mm::ExtraObjectData& extraObject) noexcept {
    extraObject.ReleaseAssociatedObject();
    if (extraObject.GetBaseObject()) {
        // If there's an object attached to this extra object, the next
        // GC sweep will have to resolve this cycle.
        extraObject.setFlag(mm::ExtraObjectData::FLAGS_FINALIZED);
    } else {
        // If there's no object attached to this extra object, the next
        // GC sweep will just collect this extra object.
        extraObject.setFlag(mm::ExtraObjectData::FLAGS_SWEEPABLE);
    }
}

// Kore: the moving young generation.

uint8_t* alloc::Allocator::allocateForEvacuation(size_t bytes) noexcept {
    return impl_->evacuationAllocator().AllocateForEvacuation(AllocationSize::bytesAtLeast(bytes));
}

void alloc::Allocator::finishEvacuation() noexcept {
    impl_->evacuationAllocator().PrepareForGC();
}

namespace {

void recordReleasedNurseryBlocks(size_t blocks) noexcept {
    if (blocks == 0) return;
    mm::GlobalData::Instance().allocator().impl().heap().allocatedSizeTracker().recordDifference(
            -static_cast<std::ptrdiff_t>(blocks * alloc::Nursery::kBlockSize));
}

} // namespace

bool alloc::nursery::enabled() noexcept {
    return Nursery::instance() != nullptr;
}

bool alloc::nursery::movable(const ObjHeader* object) noexcept {
    auto* nursery = Nursery::instance();
    return nursery != nullptr && nursery->movable(object);
}

void alloc::nursery::pin(const ObjHeader* object) noexcept {
    Nursery::instance()->pin(object);
}

bool alloc::nursery::pinned(const ObjHeader* object) noexcept {
    return Nursery::instance()->pinned(object);
}

void alloc::nursery::setAllocationEnabled(bool enabled) noexcept {
    if (auto* nursery = Nursery::instance()) nursery->setAllocationEnabled(enabled);
}

void alloc::nursery::retireAll() noexcept {
    if (Nursery::instance() == nullptr) return;
    for (auto& thread : mm::ThreadRegistry::Instance().LockForIter()) {
        thread.allocator().impl().alloc().RetireNursery();
    }
}

ObjHeader* alloc::nursery::movableObjectContaining(uintptr_t address) noexcept {
    auto* nursery = Nursery::instance();
    return nursery == nullptr ? nullptr : nursery->objectContaining(address, Nursery::BlockState::kFull);
}

void alloc::nursery::forEachMovableObject(const std::function<void(ObjHeader*)>& process) noexcept {
    if (auto* nursery = Nursery::instance()) nursery->forEachObject(Nursery::BlockState::kFull, process);
}

void alloc::nursery::retain(const ObjHeader* object) noexcept {
    Nursery::instance()->retain(object);
}

void alloc::nursery::finishYoungCollection() noexcept {
    if (auto* nursery = Nursery::instance()) recordReleasedNurseryBlocks(nursery->finishYoungCollection());
}

bool alloc::nursery::inRetainedBlock(const void* address) noexcept {
    auto* nursery = Nursery::instance();
    return nursery != nullptr && nursery->contains(address) &&
            nursery->state(nursery->blockOf(address)) == Nursery::BlockState::kRetained;
}

ObjHeader* alloc::nursery::retainedObjectContaining(uintptr_t address) noexcept {
    auto* nursery = Nursery::instance();
    return nursery == nullptr ? nullptr : nursery->objectContaining(address, Nursery::BlockState::kRetained);
}

void alloc::nursery::forEachRetainedObject(const std::function<void(ObjHeader*)>& process) noexcept {
    if (auto* nursery = Nursery::instance()) nursery->forEachObject(Nursery::BlockState::kRetained, process);
}

void alloc::nursery::releaseEmptyRetainedBlocks(const std::function<bool(ObjHeader*)>& alive) noexcept {
    if (auto* nursery = Nursery::instance()) recordReleasedNurseryBlocks(nursery->releaseEmptyRetainedBlocks(alive));
}

size_t alloc::nursery::activeBlocks() noexcept {
    auto* nursery = Nursery::instance();
    return nursery == nullptr ? 0 : nursery->countBlocks(Nursery::BlockState::kActive);
}

bool alloc::nursery::inFreeBlock(const void* address) noexcept {
    auto* nursery = Nursery::instance();
    return nursery != nullptr && nursery->contains(address) &&
            nursery->state(nursery->blockOf(address)) == Nursery::BlockState::kFree;
}
