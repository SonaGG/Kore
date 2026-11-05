/*
 * Copyright 2022 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#ifndef CUSTOM_ALLOC_CPP_NEXTFITPAGE_HPP_
#define CUSTOM_ALLOC_CPP_NEXTFITPAGE_HPP_

#include <atomic>
#include <cstdint>
#include <vector>

#include "Constants.hpp"
#include "AnyPage.hpp"
#include "AtomicStack.hpp"
#include "Cell.hpp"
#include "GCStatistics.hpp"
#include "AllocationSize.hpp"
#include "CustomLogging.hpp"
#include "CustomFinalizerProcessor.hpp"

namespace kotlin::alloc {

class alignas(kPageAlignment) NextFitPage : public MultiObjectPage<NextFitPage> {
public:
    static inline constexpr const size_t SIZE = 256 * KiB;

    static inline constexpr int cellCount() {
        return AllocationSize::bytesExactly(SIZE - sizeof(NextFitPage)).inCells();
    }

    static inline constexpr int maxBlockSize() {
        return cellCount() - 2;
    }

    static NextFitPage* Create(uint32_t cellCount) noexcept;

    void Destroy() noexcept;

    // Tries to allocate in current page, returns null if no free block in page is big enough
    uint8_t* TryAllocate(uint32_t blockSize) noexcept;

    template<typename SweepTraits>
    bool Sweep(typename SweepTraits::GCSweepScope& sweepHandle, FinalizerQueue& finalizerQueue) noexcept {
        CustomAllocDebug("NextFitPage@%p::Sweep()", this);
        Cell* end = cells_ + NextFitPage::cellCount();
        std::size_t aliveBytes = 0;
        for (Cell* block = cells_ + 1; block != end; block = block->Next()) {
            if (block->isAllocated_) {
                if (!SweepTraits::trySweepElement(block->data_, finalizerQueue, sweepHandle)) {
                    aliveBytes += AllocationSize::cells(block->size_).inBytes();
                } else {
                    block->Deallocate();
                }
            }
        }
        Cell* maxBlock = cells_; // size 0 block
        for (Cell* block = cells_ + 1; block != end; block = block->Next()) {
            if (block->isAllocated_) continue;
            for (auto* next = block->Next(); next != end; next = block->Next()) {
                if (next->isAllocated_) {
                    break;
                }
                block->size_ += next->size_;
                memset(next, 0, sizeof(*next));
            }
            if (block->size_ > maxBlock->size_) maxBlock = block;
        }
        curBlock_ = maxBlock;

        RuntimeAssert(aliveBytes == GetAllocatedSizeBytes(),
                      "Sweep counted %zu alive bytes, while GetAllocatedSizeBytes() returns %zu", aliveBytes, GetAllocatedSizeBytes());
        allocatedSizeTracker_.afterSweep(aliveBytes);
        liveFraction_ = static_cast<float>(aliveBytes) / static_cast<float>(SIZE);

        return aliveBytes > 0;
    }

    template <typename F>
    void TraverseAllocatedBlocks(F process) noexcept(noexcept(process(std::declval<uint8_t*>()))) {
        Cell* end = cells_ + cellCount();
        for (Cell* block = cells_ + 1; block != end; block = block->Next()) {
            if (block->isAllocated_) {
                process(block->data_);
            }
        }
    }

    // Kore: fraction of the page occupied by objects that survived the last sweep.
    float LiveFraction() const noexcept { return liveFraction_; }

    // Kore: first and one-past-last byte this page may hold objects in.
    const uint8_t* BlocksBegin() const noexcept { return reinterpret_cast<const uint8_t*>(cells_ + 1); }
    const uint8_t* BlocksEnd() const noexcept { return reinterpret_cast<const uint8_t*>(cells_ + cellCount()); }

    // Kore: the allocated block containing `address`, or nullptr if `address` is in a free block.
    // `address` must be in [BlocksBegin(), BlocksEnd()). Linear in the number of blocks before `address`.
    uint8_t* BlockContaining(const void* address) noexcept {
        Cell* end = cells_ + cellCount();
        for (Cell* block = cells_ + 1; block != end; block = block->Next()) {
            if (reinterpret_cast<const uint8_t*>(address) < reinterpret_cast<const uint8_t*>(block->Next())) {
                return block->isAllocated_ ? block->data_ : nullptr;
            }
        }
        return nullptr;
    }

    // Testing method
    bool CheckInvariants() noexcept;

    // Testing method
    std::vector<uint8_t*> GetAllocatedBlocks() noexcept;

private:
    explicit NextFitPage(uint32_t cellCount) noexcept;

    // Looks for a block big enough to hold cellsNeeded. If none big enough is
    // found, update to the largest one.
    void UpdateCurBlock(uint32_t cellsNeeded) noexcept;

    std::size_t GetAllocatedSizeBytes() noexcept;

    Cell* curBlock_;
    float liveFraction_ = 0; // Kore
    Cell cells_[]; // cells_[0] is reserved for an empty block
};

// ensure const-evaluatable
static_assert(NextFitPage::cellCount() > 0);

} // namespace kotlin::alloc

#endif
