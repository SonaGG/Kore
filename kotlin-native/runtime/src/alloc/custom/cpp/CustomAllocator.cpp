/*
 * Copyright 2022 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#include "CustomAllocator.hpp"

#include <cstdlib>

#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <new>

#include "AllocationHints.hpp"
#include "CustomLogging.hpp"
#include "ExtraObjectData.hpp"
#include "KAssert.h"
#include "NextFitPage.hpp"
#include "Memory.h"
#include "FixedBlockPage.hpp"
#include "GCApi.hpp"
#include "SafePoint.hpp"
#include "ThreadRegistry.hpp"

namespace kotlin::alloc {

namespace {

// Kore: allocation polls (see `Allocate`) only on threads attached to the runtime (the allocator is also used without
// one, e.g. by its tests).
ALWAYS_INLINE void pollIfAttached() noexcept {
    if (mm::ThreadRegistry::IsCurrentThreadRegistered()) mm::safePoint();
}

} // namespace

CustomAllocator::CustomAllocator(Heap& heap) noexcept : heap_(heap), nextFitPage_(nullptr), extraObjectPage_(nullptr) {
    CustomAllocInfo("CustomAllocator::CustomAllocator(heap)");
    memset(fixedBlockPages_, 0, sizeof(fixedBlockPages_));
}

CustomAllocator::~CustomAllocator() {
    heap_.AddToFinalizerQueue(std::move(finalizerQueue_));
}

ObjHeader* CustomAllocator::CreateObject(const TypeInfo* typeInfo) noexcept {
    // Kore: the common case, small enough to be inlined into the compiled code (the runtime is linked with it): bump
    // allocation in the thread's nursery block, or else in its current Immix hole. Both are zeroed. Everything else
    // (refills, objects with finalizers, other allocators) is out of line.
    if ((typeInfo->flags_ & TF_HAS_FINALIZER) == 0 && (compiler::gcNurserySize() > 0 || compiler::immix())) {
        auto descriptor = CustomHeapObject::descriptorFrom(typeInfo);
        size_t bytes = AllocationSize::bytesAtLeast(descriptor.size()).inBytes();
        uint8_t* memory = nullptr;
        if (compiler::gcNurserySize() > 0) {
            uint8_t* top = nurseryTop_;
            if (__builtin_expect(bytes <= Nursery::kMaxObjectBytes && bytes <= static_cast<size_t>(nurseryEnd_ - top), true)) {
                nurseryTop_ = top + bytes;
                memory = top;
            }
        } else {
            uint8_t* top = immixCursor_;
            if (__builtin_expect(bytes <= ImmixBlock::kMaxObjectBytes && bytes <= static_cast<size_t>(immixLimit_ - top), true)) {
                immixCursor_ = top + bytes;
                if (NeedsStartBits()) ImmixBlock::MarkStart(top);
                memory = top;
            }
        }
        if (__builtin_expect(memory != nullptr, true)) {
            type_layout::descriptor_t<gc::GC::ObjectData>::construct(memory);
            auto* object = reinterpret_cast<ObjHeader*>(memory + descriptor.fieldOffset<1>());
            object->typeInfoOrMeta_ = const_cast<TypeInfo*>(typeInfo);
            return object;
        }
    }
    return CreateObjectSlowPath(typeInfo);
}

ObjHeader* CustomAllocator::CreateObjectSlowPath(const TypeInfo* typeInfo) noexcept {
    RuntimeAssert(!typeInfo->IsArray(), "Must not be an array");
    auto descriptor = CustomHeapObject::descriptorFrom(typeInfo);
    auto size = AllocationSize::bytesAtLeast(descriptor.size());
    bool hasFinalizer = typeInfo->flags_ & TF_HAS_FINALIZER;
    // Kore: young objects go to the nursery if there is one (survivors are then copied to Immix blocks or pages).
    // Objects with finalizers never move (their extra object data points back to them).
    uint8_t* memory = compiler::gcNurserySize() > 0 && !hasFinalizer ? TryAllocateInNursery(size) : nullptr;
    if (memory == nullptr) memory = compiler::immix() ? AllocateInImmix(size, /* poll = */ true) : Allocate(size);
    auto& heapObject = *descriptor.construct(memory);
    ObjHeader* object = heapObject.object();
    if (hasFinalizer) {
        auto* extraObject = CreateExtraObjectDataForObject(object, typeInfo);
        object->typeInfoOrMeta_ = reinterpret_cast<TypeInfo*>(extraObject);
        nurseryKeepInPlace(object); // Kore: finalized where it is (docs/kore/immix-nursery.md).
        CustomAllocDebug("CustomAllocator: %p gets extraObject %p", object, extraObject);
        CustomAllocDebug("CustomAllocator: %p->BaseObject == %p", extraObject, extraObject->GetBaseObject());
    } else {
        object->typeInfoOrMeta_ = const_cast<TypeInfo*>(typeInfo);
    }
    return object;
}

ArrayHeader* CustomAllocator::CreateArray(const TypeInfo* typeInfo, uint32_t count) noexcept {
    // Kore: as CreateObject: small arrays are bump-allocated in the current Immix hole (zeroed), inlined into the
    // compiled code; everything else (refills, the nursery, large arrays and their hints) is out of line.
    if (compiler::immix() && compiler::gcNurserySize() == 0) {
        auto descriptor = CustomHeapArray::descriptorFrom(typeInfo, count);
        uint64_t size = descriptor.size();
        uint8_t* top = immixCursor_;
        if (__builtin_expect(size <= ImmixBlock::kMaxObjectBytes, true)) {
            size_t bytes = AllocationSize::bytesAtLeast(size).inBytes();
            if (__builtin_expect(bytes <= static_cast<size_t>(immixLimit_ - top), true)) {
                immixCursor_ = top + bytes;
                if (NeedsStartBits()) ImmixBlock::MarkStart(top);
                auto& heapArray = *descriptor.construct(top);
                ArrayHeader* array = heapArray.array();
                array->typeInfoOrMeta_ = const_cast<TypeInfo*>(typeInfo);
                array->count_ = count;
                return array;
            }
        }
    }
    return CreateArraySlowPath(typeInfo, count);
}

ArrayHeader* CustomAllocator::CreateArraySlowPath(const TypeInfo* typeInfo, uint32_t count) noexcept {
    CustomAllocDebug("CustomAllocator@%p::CreateArray(%d)", this ,count);
    RuntimeAssert(typeInfo->IsArray(), "Must be an array");
    auto descriptor = CustomHeapArray::descriptorFrom(typeInfo, count);
    auto size = AllocationSize::bytesAtLeast(descriptor.size());
    // Kore: a large array's elements that the caller overwrites need not be zeroed (see `tlsArrayOverwrittenPrefixBytes`).
    // (Not with runtime assertions, which check that the memory of new objects is zeroed.)
    size_t overwritten = tlsArrayOverwrittenPrefixBytes;
    if (overwritten > 0 && compiler::runtimeAssertsMode() == compiler::RuntimeAssertsMode::kIgnore) {
        constexpr uintptr_t kProbe = 4096; // The offset of the elements, computed on an aligned address.
        auto elements = reinterpret_cast<uintptr_t>(reinterpret_cast<CustomHeapArray*>(kProbe)->array()) - kProbe + sizeof(ArrayHeader);
        tlsSingleObjectPageUnzeroed = {static_cast<size_t>(elements), overwritten};
    }
    uint8_t* memory = compiler::gcNurserySize() > 0 ? TryAllocateInNursery(size) : nullptr;
    if (memory == nullptr) memory = compiler::immix() ? AllocateInImmix(size, /* poll = */ true) : Allocate(size);
    tlsSingleObjectPageUnzeroed = {};
    auto& heapArray = *descriptor.construct(memory);
    ArrayHeader* array = heapArray.array();
    array->typeInfoOrMeta_ = const_cast<TypeInfo*>(typeInfo);
    array->count_ = count;
    return array;
}

mm::ExtraObjectData* CustomAllocator::CreateExtraObjectDataForObject(ObjHeader* baseObject, const TypeInfo* type) noexcept {
    // Kore: the object never moves from now on (docs/kore/immix-nursery.md). Not while it is being created (no header yet):
    // CreateObjectSlowPath does it then.
    if (baseObject->typeInfoOrMeta_ != nullptr) nurseryKeepInPlace(baseObject);
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
    // Kore: and poll, so that a thread that allocates (and may have requested the collection by doing so) stops soon
    // even in code without other polls. Allocations are GC points for the compiled code.
    pollIfAttached();
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

uint8_t* CustomAllocator::Allocate(AllocationSize size, bool poll) noexcept {
    RuntimeAssert(size > AllocationSize::cells(0), "CustomAllocator::Allocate cannot allocate 0 bytes");
    CustomAllocDebug("CustomAllocator::Allocate(%" PRIu64 ")", size.inBytes());
    uint64_t cellCount = size.inCells();
    if (compiler::immix()) {
        if (size.inBytes() <= ImmixBlock::kMaxObjectBytes) return AllocateInImmix(size, poll);
        return AllocateLargerThanImmix(cellCount, poll);
    }
    if (!compiler::pagedAllocator()) {
        return AllocateInSingleObjectPage(cellCount, poll);
    }
    if (cellCount <= FixedBlockPage::MAX_BLOCK_SIZE) {
        return AllocateInFixedBlockPage(cellCount, poll);
    }
    if (cellCount > NextFitPage::maxBlockSize()) {
        return AllocateInSingleObjectPage(cellCount, poll);
    }
    return AllocateInNextFitPage(cellCount, poll);
}

uint8_t* CustomAllocator::AllocateLargerThanImmix(uint64_t cellCount, bool poll) noexcept {
    // Kore: objects of up to a next-fit page stay in next-fit pages. Giving each of them a single-object page (system
    // memory per object) made array-growing code slower and the young pauses longer: resolving remembered slots and
    // conservative references walks every page.
    if (compiler::pagedAllocator() && cellCount <= static_cast<uint64_t>(NextFitPage::maxBlockSize())) {
        return AllocateInNextFitPage(static_cast<uint32_t>(cellCount), poll);
    }
    return AllocateInSingleObjectPage(cellCount, poll);
}

void CustomAllocator::ResetImmix() noexcept {
    SealNursery();
    nurseryBlock_ = nullptr;
    immixCursor_ = immixLimit_ = nullptr;
    immixBlock_ = nullptr;
    immixNextLine_ = 0;
    overflowCursor_ = overflowLimit_ = nullptr;
}

void CustomAllocator::TakeImmixHole(ImmixBlock* block, uint8_t* start, uint8_t* end, uint8_t*& cursor, uint8_t*& limit) noexcept {
    if (__builtin_expect(immixCheck(), false)) block->CheckNoStartsIn(start, end, evacuation_ ? "evacuation hole" : "hole");
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

uint8_t* CustomAllocator::AllocateInImmixSlowPath(AllocationSize size, bool poll) noexcept {
    size_t bytes = size.inBytes();
    if (bytes > ImmixBlock::kMaxObjectBytes) return AllocateLargerThanImmix(size.inCells(), poll);
    // Kore: medium objects don't skip holes that are too small for them (small objects will fill them).
    if (bytes > ImmixBlock::kLineSize) return AllocateInImmixOverflow(bytes, poll);
    while (true) {
        uint8_t* top = immixCursor_;
        if (bytes <= static_cast<size_t>(immixLimit_ - top)) {
            immixCursor_ = top + bytes;
            if (NeedsStartBits()) ImmixBlock::MarkStart(top);
            return top;
        }
        // Kore: the nursery (docs/kore/immix-nursery.md): a mutator bumps through whole free blocks.
        if (compiler::immixNursery() && !evacuation_) {
            if (poll) {
                pollIfAttached();
                if (bytes <= static_cast<size_t>(immixLimit_ - immixCursor_)) continue;
            }
            SealNursery();
            ImmixBlock* block = heap_.immixBlocks().TakeFreeBlock();
            block->SetNursery(true);
            immixBlock_ = nullptr;
            nurseryBlock_ = block;
            TakeImmixHole(block, block->Begin(), block->End(), immixCursor_, immixLimit_);
            continue;
        }
        uint8_t* start;
        uint8_t* end;
        if (immixBlock_ == nullptr || !immixBlock_->NextHole(immixNextLine_, start, end)) {
            // Kore: see `Allocate`: a mutator polls before taking a new block (the loop then rereads the fields, which a
            // collection resets).
            if (poll) {
                pollIfAttached();
                if (bytes <= static_cast<size_t>(immixLimit_ - immixCursor_)) continue;
            }
            immixBlock_ = heap_.immixBlocks().TakeBlock();
            immixNextLine_ = ImmixBlock::FirstLine();
            bool found = immixBlock_->NextHole(immixNextLine_, start, end);
            RuntimeAssert(found, "Immix block %p from the heap has no hole", immixBlock_);
        }
        TakeImmixHole(immixBlock_, start, end, immixCursor_, immixLimit_);
    }
}

uint8_t* CustomAllocator::AllocateInImmixOverflow(size_t bytes, bool poll) noexcept {
    while (true) {
        uint8_t* top = overflowCursor_;
        if (bytes <= static_cast<size_t>(overflowLimit_ - top)) {
            overflowCursor_ = top + bytes;
            ImmixBlock::MarkStart(top);
            return top;
        }
        // The rest of the previous overflow block (less than a medium object) is left to the next sweep.
        if (poll) {
            pollIfAttached();
            if (bytes <= static_cast<size_t>(overflowLimit_ - overflowCursor_)) continue;
        }
        ImmixBlock* block = heap_.immixBlocks().TakeFreeBlock();
        if (compiler::immixNursery() && !evacuation_) block->SetNursery(true);
        TakeImmixHole(block, block->Begin(), block->End(), overflowCursor_, overflowLimit_);
    }
}

uint8_t* CustomAllocator::AllocateInSingleObjectPage(uint64_t cellCount, bool poll) noexcept {
    CustomAllocDebug("CustomAllocator::AllocateInSingleObjectPage(%" PRIu64 ")", cellCount);
    // Kore: see `Allocate`. Large objects are rare, and slow to allocate anyway.
    if (poll) pollIfAttached();
    uint8_t* block = heap_.GetSingleObjectPage(cellCount)->Allocate();
    return block;
}

uint8_t* CustomAllocator::AllocateInNextFitPage(uint32_t cellCount, bool poll) noexcept {
    CustomAllocDebug("CustomAllocator::AllocateInNextFitPage(%u)", cellCount);
    if (nextFitPage_) {
        uint8_t* block = nextFitPage_->TryAllocate(cellCount);
        if (block) return block;
    }
    CustomAllocDebug("Failed to allocate in curPage");
    if (poll) pollIfAttached(); // Kore: see `Allocate`.
    while (true) {
        nextFitPage_ = heap_.GetNextFitPage(cellCount, finalizerQueue_);
        uint8_t* block = nextFitPage_->TryAllocate(cellCount);
        if (block) return block;
    }
}

uint8_t* CustomAllocator::AllocateInFixedBlockPage(uint32_t cellCount, bool poll) noexcept {
    CustomAllocDebug("CustomAllocator::AllocateInFixedBlockPage(%u)", cellCount);
    FixedBlockPage* page = fixedBlockPages_[cellCount];
    if (page) {
        uint8_t* block = page->TryAllocate();
        if (block) return block;
    }
    CustomAllocDebug("Failed to allocate in current FixedBlockPage");
    if (poll) pollIfAttached(); // Kore: see `Allocate`.
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
