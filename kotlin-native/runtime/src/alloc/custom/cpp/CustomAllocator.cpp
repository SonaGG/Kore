/*
 * Copyright 2022 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#include "CustomAllocator.hpp"

#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <new>

#include "CustomLogging.hpp"
#include "ExtraObjectData.hpp"
#include "KAssert.h"
#include "NextFitPage.hpp"
#include "Memory.h"
#include "FixedBlockPage.hpp"
#include "GCApi.hpp"

namespace kotlin::alloc {

CustomAllocator::CustomAllocator(Heap& heap) noexcept : heap_(heap), nextFitPage_(nullptr), extraObjectPage_(nullptr) {
    CustomAllocInfo("CustomAllocator::CustomAllocator(heap)");
    memset(fixedBlockPages_, 0, sizeof(fixedBlockPages_));
}

CustomAllocator::~CustomAllocator() {
    heap_.AddToFinalizerQueue(std::move(finalizerQueue_));
}

ObjHeader* CustomAllocator::CreateObject(const TypeInfo* typeInfo) noexcept {
    RuntimeAssert(!typeInfo->IsArray(), "Must not be an array");
    auto descriptor = CustomHeapObject::descriptorFrom(typeInfo);
    auto size = AllocationSize::bytesAtLeast(descriptor.size());
    bool hasFinalizer = typeInfo->flags_ & TF_HAS_FINALIZER;
    uint8_t* memory;
    if (compiler::immix()) {
        memory = AllocateInImmix(size);
    } else {
        // Kore: objects with finalizers never move (their extra object data points back to them).
        memory = hasFinalizer ? nullptr : TryAllocateInNursery(size);
        if (memory == nullptr) memory = Allocate(size);
    }
    auto& heapObject = *descriptor.construct(memory);
    ObjHeader* object = heapObject.object();
    if (hasFinalizer) {
        auto* extraObject = CreateExtraObjectDataForObject(object, typeInfo);
        object->typeInfoOrMeta_ = reinterpret_cast<TypeInfo*>(extraObject);
        CustomAllocDebug("CustomAllocator: %p gets extraObject %p", object, extraObject);
        CustomAllocDebug("CustomAllocator: %p->BaseObject == %p", extraObject, extraObject->GetBaseObject());
    } else {
        object->typeInfoOrMeta_ = const_cast<TypeInfo*>(typeInfo);
    }
    return object;
}

ArrayHeader* CustomAllocator::CreateArray(const TypeInfo* typeInfo, uint32_t count) noexcept {
    CustomAllocDebug("CustomAllocator@%p::CreateArray(%d)", this ,count);
    RuntimeAssert(typeInfo->IsArray(), "Must be an array");
    auto descriptor = CustomHeapArray::descriptorFrom(typeInfo, count);
    auto size = AllocationSize::bytesAtLeast(descriptor.size());
    uint8_t* memory;
    if (compiler::immix()) {
        memory = AllocateInImmix(size);
    } else {
        memory = TryAllocateInNursery(size);
        if (memory == nullptr) memory = Allocate(size);
    }
    auto& heapArray = *descriptor.construct(memory);
    ArrayHeader* array = heapArray.array();
    array->typeInfoOrMeta_ = const_cast<TypeInfo*>(typeInfo);
    array->count_ = count;
    return array;
}

mm::ExtraObjectData* CustomAllocator::CreateExtraObjectDataForObject(ObjHeader* baseObject, const TypeInfo* type) noexcept {
    CustomAllocDebug("CustomAllocator@%p::CreateExtraObjectDataForObject(%p)", this ,baseObject);
    auto* extraObjectCell = reinterpret_cast<ExtraObjectCell*>(AllocateExtraObjectData());
    return new (extraObjectCell->data_) mm::ExtraObjectData(baseObject, type);
}

FinalizerQueue CustomAllocator::ExtractFinalizerQueue() noexcept {
    return std::move(finalizerQueue_);
}

void CustomAllocator::RetireNursery() noexcept {
    if (nurseryStart_ == nullptr) return;
    Nursery::instance()->retire(nurseryStart_, nurseryTop_);
    nurseryStart_ = nurseryTop_ = nurseryEnd_ = nullptr;
}

uint8_t* CustomAllocator::RefillNursery(size_t bytes) noexcept {
    if (bytes > Nursery::kMaxObjectBytes) return nullptr;
    RetireNursery();
    // Notify first: the scheduler may suspend this thread for a collection, which must not see a block owned by it
    // (with an object not constructed yet), and may free the blocks.
    heap_.allocatedSizeTracker().recordDifferenceAndNotifyScheduler(static_cast<std::ptrdiff_t>(Nursery::kBlockSize));
    uint8_t* start;
    uint8_t* end;
    if (!Nursery::instance()->takeBlock(start, end)) {
        heap_.allocatedSizeTracker().recordDifference(-static_cast<std::ptrdiff_t>(Nursery::kBlockSize));
        return nullptr;
    }
    nurseryStart_ = start;
    nurseryTop_ = start + bytes;
    nurseryEnd_ = end;
    return start;
}

void CustomAllocator::PrepareForGC() noexcept {
    CustomAllocInfo("CustomAllocator@%p::PrepareForGC()", this);
    RetireNursery();
    ResetImmix();
    nextFitPage_ = nullptr;
    memset(fixedBlockPages_, 0, sizeof(fixedBlockPages_));
    extraObjectPage_ = nullptr;
}

// static
size_t CustomAllocator::GetAllocatedHeapSize(ObjHeader* object) noexcept {
    return CustomHeapObject::from(object).size();
}

uint8_t* CustomAllocator::Allocate(AllocationSize size) noexcept {
    RuntimeAssert(size > AllocationSize::cells(0), "CustomAllocator::Allocate cannot allocate 0 bytes");
    CustomAllocDebug("CustomAllocator::Allocate(%" PRIu64 ")", size.inBytes());
    uint64_t cellCount = size.inCells();
    if (compiler::immix()) {
        if (size.inBytes() <= ImmixBlock::kMaxObjectBytes) return AllocateInImmix(size);
        return AllocateLargerThanImmix(cellCount);
    }
    if (!compiler::pagedAllocator()) {
        return AllocateInSingleObjectPage(cellCount);
    }
    if (cellCount <= FixedBlockPage::MAX_BLOCK_SIZE) {
        return AllocateInFixedBlockPage(cellCount);
    }
    if (cellCount > NextFitPage::maxBlockSize()) {
        return AllocateInSingleObjectPage(cellCount);
    }
    return AllocateInNextFitPage(cellCount);
}

uint8_t* CustomAllocator::AllocateLargerThanImmix(uint64_t cellCount) noexcept {
    // Kore: objects of up to a next-fit page stay in next-fit pages. Giving each of them a single-object page (system
    // memory per object) made array-growing code slower and the young pauses longer: resolving remembered slots and
    // conservative references walks every page.
    if (compiler::pagedAllocator() && cellCount <= static_cast<uint64_t>(NextFitPage::maxBlockSize())) {
        return AllocateInNextFitPage(static_cast<uint32_t>(cellCount));
    }
    return AllocateInSingleObjectPage(cellCount);
}

void CustomAllocator::ResetImmix() noexcept {
    immixCursor_ = immixLimit_ = nullptr;
    immixBlock_ = nullptr;
    immixNextLine_ = 0;
    overflowCursor_ = overflowLimit_ = nullptr;
}

void CustomAllocator::TakeImmixHole(ImmixBlock* block, uint8_t* start, uint8_t* end, uint8_t*& cursor, uint8_t*& limit) noexcept {
    if (!block->Zeroed()) std::memset(start, 0, end - start);
    block->SetUsed();
    cursor = start;
    limit = end;
    auto bytes = static_cast<size_t>(end - start);
    block->RecordedBytes() += bytes;
    // Notify last: the scheduler may suspend this thread for a collection, whose `PrepareForGC` drops the hole (the
    // block is swept with the others allocated into). Callers retry from the allocator's fields afterwards.
    heap_.allocatedSizeTracker().recordDifferenceAndNotifyScheduler(static_cast<std::ptrdiff_t>(bytes));
}

uint8_t* CustomAllocator::AllocateInImmixSlowPath(AllocationSize size) noexcept {
    size_t bytes = size.inBytes();
    if (bytes > ImmixBlock::kMaxObjectBytes) return AllocateLargerThanImmix(size.inCells());
    // Kore: medium objects don't skip holes that are too small for them (small objects will fill them).
    if (bytes > ImmixBlock::kLineSize) return AllocateInImmixOverflow(bytes);
    while (true) {
        uint8_t* top = immixCursor_;
        if (bytes <= static_cast<size_t>(immixLimit_ - top)) {
            immixCursor_ = top + bytes;
            ImmixBlock::MarkStart(top);
            return top;
        }
        uint8_t* start;
        uint8_t* end;
        if (immixBlock_ == nullptr || !immixBlock_->NextHole(immixNextLine_, start, end)) {
            immixBlock_ = heap_.immixBlocks().TakeBlock();
            immixNextLine_ = ImmixBlock::FirstLine();
            bool found = immixBlock_->NextHole(immixNextLine_, start, end);
            RuntimeAssert(found, "Immix block %p from the heap has no hole", immixBlock_);
        }
        TakeImmixHole(immixBlock_, start, end, immixCursor_, immixLimit_);
    }
}

uint8_t* CustomAllocator::AllocateInImmixOverflow(size_t bytes) noexcept {
    while (true) {
        uint8_t* top = overflowCursor_;
        if (bytes <= static_cast<size_t>(overflowLimit_ - top)) {
            overflowCursor_ = top + bytes;
            ImmixBlock::MarkStart(top);
            return top;
        }
        // The rest of the previous overflow block (less than a medium object) is left to the next sweep.
        ImmixBlock* block = heap_.immixBlocks().TakeFreeBlock();
        TakeImmixHole(block, block->Begin(), block->End(), overflowCursor_, overflowLimit_);
    }
}

uint8_t* CustomAllocator::AllocateInSingleObjectPage(uint64_t cellCount) noexcept {
    CustomAllocDebug("CustomAllocator::AllocateInSingleObjectPage(%" PRIu64 ")", cellCount);
    uint8_t* block = heap_.GetSingleObjectPage(cellCount)->Allocate();
    return block;
}

uint8_t* CustomAllocator::AllocateInNextFitPage(uint32_t cellCount) noexcept {
    CustomAllocDebug("CustomAllocator::AllocateInNextFitPage(%u)", cellCount);
    if (nextFitPage_) {
        uint8_t* block = nextFitPage_->TryAllocate(cellCount);
        if (block) return block;
    }
    CustomAllocDebug("Failed to allocate in curPage");
    while (true) {
        nextFitPage_ = heap_.GetNextFitPage(cellCount, finalizerQueue_);
        uint8_t* block = nextFitPage_->TryAllocate(cellCount);
        if (block) return block;
    }
}

uint8_t* CustomAllocator::AllocateInFixedBlockPage(uint32_t cellCount) noexcept {
    CustomAllocDebug("CustomAllocator::AllocateInFixedBlockPage(%u)", cellCount);
    FixedBlockPage* page = fixedBlockPages_[cellCount];
    if (page) {
        uint8_t* block = page->TryAllocate();
        if (block) return block;
    }
    CustomAllocDebug("Failed to allocate in current FixedBlockPage");
    while ((page = heap_.GetFixedBlockPage(cellCount, finalizerQueue_))) {
        uint8_t* block = page->TryAllocate();
        if (block) {
            fixedBlockPages_[cellCount] = page;
            return block;
        }
    }
    return nullptr;
}

uint8_t* CustomAllocator::AllocateExtraObjectData() noexcept {
    CustomAllocDebug("CustomAllocator::AllocateExtraObjectData()");

    if (!compiler::pagedAllocator()) {
        return heap_.GetSingleExtraObjectPage()->Allocate();
    }

    auto* page = extraObjectPage_;
    if (page) {
        uint8_t* block = page->TryAllocate();
        if (block) return block;
    }

    CustomAllocDebug("Failed to allocate in current ExtraObjectPage");
    while ((page = heap_.GetFixedBlockExtraObjectPage(finalizerQueue_))) {
        uint8_t* block = page->TryAllocate();
        if (block) {
            extraObjectPage_ = page;
            return block;
        }
    }
    return nullptr;
}

} // namespace kotlin::alloc
