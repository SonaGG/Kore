/*
 * Copyright 2022 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#ifndef CUSTOM_ALLOC_CPP_ALLOCATOR_HPP_
#define CUSTOM_ALLOC_CPP_ALLOCATOR_HPP_

#include <atomic>
#include <cstring>

#include "AllocationSize.hpp"
#include "CompilerConstants.hpp"
#include "ExtraObjectData.hpp"
#include "Heap.hpp"
#include "ImmixBlock.hpp"
#include "Nursery.hpp"
#include "NextFitPage.hpp"
#include "Memory.h"
#include "FixedBlockPage.hpp"

namespace kotlin::alloc {

class CustomAllocator {
public:
    explicit CustomAllocator(Heap& heap) noexcept;

    ~CustomAllocator();

    ObjHeader* CreateObject(const TypeInfo* typeInfo) noexcept;

    ArrayHeader* CreateArray(const TypeInfo* typeInfo, uint32_t count) noexcept;

    mm::ExtraObjectData* CreateExtraObjectDataForObject(ObjHeader* baseObject, const TypeInfo* type) noexcept;

    void PrepareForGC() noexcept;

    FinalizerQueue ExtractFinalizerQueue() noexcept;

    static size_t GetAllocatedHeapSize(ObjHeader* object) noexcept;

    // Kore: ends the thread's nursery allocation block (see Nursery.hpp).
    void RetireNursery() noexcept;

    // Kore: memory in the regular heap for an object moved out of the nursery.
    uint8_t* AllocateForEvacuation(AllocationSize size) noexcept { return Allocate(size, /* poll = */ false); }

    Heap& heap() noexcept {
        return heap_;
    }

private:
    // Kore: with `poll`, a mutator polls before taking a new page or allocating a large object (not the GC thread,
    // which evacuates with it).
    uint8_t* Allocate(AllocationSize size, bool poll = true) noexcept;
    uint8_t* AllocateInSingleObjectPage(uint64_t cellCount, bool poll) noexcept;
    uint8_t* AllocateInNextFitPage(uint32_t cellCount, bool poll) noexcept;
    uint8_t* AllocateInFixedBlockPage(uint32_t cellCount, bool poll) noexcept;
    uint8_t* AllocateExtraObjectData() noexcept;

    // Kore: bump allocation in the thread's nursery block, or null.
    ALWAYS_INLINE uint8_t* TryAllocateInNursery(AllocationSize size) noexcept {
        size_t bytes = size.inBytes();
        uint8_t* top = nurseryTop_;
        if (__builtin_expect(bytes <= Nursery::kMaxObjectBytes && bytes <= static_cast<size_t>(nurseryEnd_ - top), true)) {
            nurseryTop_ = top + bytes;
            return top;
        }
        if (Nursery::instance() == nullptr) return nullptr;
        return RefillNursery(bytes);
    }
    NO_INLINE uint8_t* RefillNursery(size_t bytes) noexcept;

    // Kore: memory v2. Bump allocation in the current hole of the thread's Immix block.
    ALWAYS_INLINE uint8_t* AllocateInImmix(AllocationSize size, bool poll) noexcept {
        size_t bytes = size.inBytes();
        uint8_t* top = immixCursor_;
        if (__builtin_expect(bytes <= ImmixBlock::kMaxObjectBytes && bytes <= static_cast<size_t>(immixLimit_ - top), true)) {
            immixCursor_ = top + bytes;
            ImmixBlock::MarkStart(top);
            return top;
        }
        return AllocateInImmixSlowPath(size, poll);
    }
    NO_INLINE uint8_t* AllocateInImmixSlowPath(AllocationSize size, bool poll) noexcept;
    uint8_t* AllocateInImmixOverflow(size_t bytes, bool poll) noexcept;
    uint8_t* AllocateLargerThanImmix(uint64_t cellCount, bool poll) noexcept;
    // Makes [start, end) the hole bumped through by `cursor` up to `limit`.
    void TakeImmixHole(ImmixBlock* block, uint8_t* start, uint8_t* end, uint8_t*& cursor, uint8_t*& limit) noexcept;
    void ResetImmix() noexcept;

    Heap& heap_;
    NextFitPage* nextFitPage_;
    FixedBlockPage* fixedBlockPages_[FixedBlockPage::MAX_BLOCK_SIZE + 1];
    FixedBlockPage* extraObjectPage_;
    FinalizerQueue finalizerQueue_;
    uint8_t* nurseryStart_ = nullptr;
    uint8_t* nurseryTop_ = nullptr;
    uint8_t* nurseryEnd_ = nullptr;
    // Kore: Immix allocation (see ImmixBlock.hpp). Small objects go to holes of `immixBlock_`, medium objects that do
    // not fit the current hole go to a separate overflow block (Immix "overflow allocation").
    uint8_t* immixCursor_ = nullptr;
    uint8_t* immixLimit_ = nullptr;
    ImmixBlock* immixBlock_ = nullptr;
    size_t immixNextLine_ = 0;
    uint8_t* overflowCursor_ = nullptr;
    uint8_t* overflowLimit_ = nullptr;
};

} // namespace kotlin::alloc

#endif
