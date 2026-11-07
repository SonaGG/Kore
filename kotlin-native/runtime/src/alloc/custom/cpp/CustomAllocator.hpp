/*
 * Copyright 2022 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#ifndef CUSTOM_ALLOC_CPP_ALLOCATOR_HPP_
#define CUSTOM_ALLOC_CPP_ALLOCATOR_HPP_

#include <atomic>
#include <cstring>

#include "AllocationSize.hpp"
#include "Common.h"
#include "CompilerConstants.hpp"
#include "ExtraObjectData.hpp"
#include "Heap.hpp"
#include "ImmixBlock.hpp"
#include "Nursery.hpp"
#include "NextFitPage.hpp"
#include "Memory.h"
#include "FixedBlockPage.hpp"

namespace kotlin::alloc {

// Kore: the hole an allocator bumps through.
struct ImmixBump {
    uint8_t* cursor = nullptr;
    uint8_t* limit = nullptr;
};

// Kore: the hole of the current thread's mutator allocator (see `CustomAllocator::UseThreadBump`). The inlined fast
// paths read it from thread-local storage directly, without the chain of dependent loads from the thread data to the
// allocator. Another allocator on the thread finds it empty or not its own; either way its objects are in the heap.
extern THREAD_LOCAL_VARIABLE ImmixBump tMutatorBump;

class CustomAllocator {
public:
    explicit CustomAllocator(Heap& heap) noexcept;

    ~CustomAllocator();

    ObjHeader* CreateObject(const TypeInfo* typeInfo) noexcept;

    ArrayHeader* CreateArray(const TypeInfo* typeInfo, uint32_t count) noexcept;
    NO_INLINE ArrayHeader* CreateArraySlowPath(const TypeInfo* typeInfo, uint32_t count) noexcept;

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

    // Kore: the GC's allocator for evacuated objects (not a mutator's: it never allocates into the nursery).
    void SetEvacuation() noexcept { evacuation_ = true; }

    // Kore: makes this (the current thread's mutator allocator) bump through `tMutatorBump`.
    void UseThreadBump() noexcept {
        tMutatorBump = *bump_;
        bump_ = &tMutatorBump;
    }

    // Kore: `NeedsStartBits` of a mutator's allocator (never an evacuation one).
    static ALWAYS_INLINE bool MutatorNeedsStartBits() noexcept { return !compiler::immixNursery() || nurseryStartBits(); }

    // Kore: the hole of the inlined fast paths (`CreateObject`, `CreateArray`): `tMutatorBump`, read without `this`.
    ALWAYS_INLINE ImmixBump& FastBump() noexcept {
#if defined(__APPLE__)
        return *bump_;
#else
        return tMutatorBump;
#endif
    }

    // Kore: records where the objects of the current nursery block end (docs/kore/immix-nursery.md).
    void SealNursery() noexcept {
        if (nurseryBlock_ != nullptr) nurseryBlock_->SetNurseryTop(bump_->cursor);
    }

    // Kore: whether objects bumped into the current hole need their start bit (not in the nursery, see above).
    ALWAYS_INLINE bool NeedsStartBits() const noexcept {
        return !compiler::immixNursery() || evacuation_ || nurseryStartBits();
    }

private:
    // Kore: with `poll`, a mutator polls before taking a new page or allocating a large object (not the GC thread,
    // which evacuates with it).
    uint8_t* Allocate(AllocationSize size, bool poll = true) noexcept;
    uint8_t* AllocateInSingleObjectPage(uint64_t cellCount, bool poll) noexcept;
    uint8_t* AllocateInNextFitPage(uint32_t cellCount, bool poll) noexcept;
    uint8_t* AllocateInFixedBlockPage(uint32_t cellCount, bool poll) noexcept;
    uint8_t* AllocateExtraObjectData() noexcept;

    NO_INLINE ObjHeader* CreateObjectSlowPath(const TypeInfo* typeInfo) noexcept;

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
        uint8_t* top = bump_->cursor;
        if (__builtin_expect(bytes <= ImmixBlock::kMaxObjectBytes && bytes <= static_cast<size_t>(bump_->limit - top), true)) {
            bump_->cursor = top + bytes;
            if (NeedsStartBits()) ImmixBlock::MarkStart(top);
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
    ImmixBump ownBump_;
    ImmixBump* bump_ = &ownBump_; // Kore: `&ownBump_` or `&tMutatorBump` (see `UseThreadBump`).
    ImmixBlock* immixBlock_ = nullptr;
    size_t immixNextLine_ = 0;
    uint8_t* overflowCursor_ = nullptr;
    uint8_t* overflowLimit_ = nullptr;
    bool evacuation_ = false;
    ImmixBlock* nurseryBlock_ = nullptr; // Kore: the nursery block `bump_` bumps through, if any.
};

} // namespace kotlin::alloc

#endif
