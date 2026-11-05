/*
 * Copyright 2022 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#ifndef CUSTOM_ALLOC_CPP_ALLOCATOR_HPP_
#define CUSTOM_ALLOC_CPP_ALLOCATOR_HPP_

#include <atomic>
#include <cstring>

#include "AllocationSize.hpp"
#include "ExtraObjectData.hpp"
#include "Heap.hpp"
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
    uint8_t* AllocateForEvacuation(AllocationSize size) noexcept { return Allocate(size); }

    Heap& heap() noexcept {
        return heap_;
    }

private:
    uint8_t* Allocate(AllocationSize size) noexcept;
    uint8_t* AllocateInSingleObjectPage(uint64_t cellCount) noexcept;
    uint8_t* AllocateInNextFitPage(uint32_t cellCount) noexcept;
    uint8_t* AllocateInFixedBlockPage(uint32_t cellCount) noexcept;
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

    Heap& heap_;
    NextFitPage* nextFitPage_;
    FixedBlockPage* fixedBlockPages_[FixedBlockPage::MAX_BLOCK_SIZE + 1];
    FixedBlockPage* extraObjectPage_;
    FinalizerQueue finalizerQueue_;
    uint8_t* nurseryStart_ = nullptr;
    uint8_t* nurseryTop_ = nullptr;
    uint8_t* nurseryEnd_ = nullptr;
};

} // namespace kotlin::alloc

#endif
