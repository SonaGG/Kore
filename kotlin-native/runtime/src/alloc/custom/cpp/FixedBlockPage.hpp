/*
 * Copyright 2022 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#ifndef CUSTOM_ALLOC_CPP_FIXEDBLOCKPAGE_HPP_
#define CUSTOM_ALLOC_CPP_FIXEDBLOCKPAGE_HPP_

#include <atomic>
#include <cstdint>
#include <vector>

#include "Constants.hpp"
#include "AtomicStack.hpp"
#include "GCStatistics.hpp"
#include "AnyPage.hpp"
#include "CustomLogging.hpp"
#include "GCApi.hpp"

namespace kotlin::alloc {

struct alignas(8) FixedCellRange {
    uint32_t first;
    uint32_t last;
};

struct alignas(8) FixedBlockCell {
    // The FixedBlockCell either contains data or a pointer to the next free cell
    union {
        uint8_t data[];
        FixedCellRange nextFree;
    };
};

class alignas(32) FixedBlockPage : public MultiObjectPage<FixedBlockPage> { // Kore: 32, see `cells_`.
public:
    static inline constexpr size_t SIZE() {
        return kotlin::compiler::fixedBlockPageSize() * KiB;
    }

    static inline constexpr const int MAX_BLOCK_SIZE = 128;

    static inline constexpr size_t cellCount() {
        return (SIZE() - sizeof(FixedBlockPage)) / sizeof(FixedBlockCell);
    }

    static FixedBlockPage* Create(uint32_t blockSize) noexcept;

    void Destroy() noexcept;

    // Tries to allocate in current page, returns null if no free block in page
    uint8_t* TryAllocate() noexcept;

    template<typename SweepTraits>
    bool Sweep(typename SweepTraits::GCSweepScope& sweepHandle, FinalizerQueue& finalizerQueue) noexcept {
        CustomAllocInfo("FixedBlockPage(%p)::Sweep()", this);
        FixedCellRange nextFree = nextFree_; // Accessing the previous free list structure.
        FixedCellRange* prevRange = &nextFree_; // Creating the new free list structure.
        uint32_t prevLive = -blockSize_;
        std::size_t aliveBlocksCount = 0;
        for (uint32_t cell = 0 ; cell < end_ ; cell += blockSize_) {
            // Go through the occupied cells.
            for (; cell < nextFree.first ; cell += blockSize_) {
                if (SweepTraits::trySweepElement(cells_[cell].data, finalizerQueue, sweepHandle)) {
                    // We should null this cell out, but we will do so in batch later.
                    continue;
                }
                ++aliveBlocksCount;
                if (prevLive + blockSize_ < cell) {
                    // We found an alive cell that ended a run of swept cells or a known unoccupied range.
                    uint32_t prevCell = cell - blockSize_;
                    // Nulling in batch.
                    memset(&cells_[prevLive + blockSize_], 0, (prevCell - prevLive) * sizeof(FixedBlockCell));
                    // Updating the free list structure.
                    prevRange->first = prevLive + blockSize_;
                    prevRange->last = prevCell;
                    // And the next unoccupied range will be stored in the last unoccupied cell.
                    prevRange = &cells_[prevCell].nextFree;
                }
                prevLive = cell;
            }
            // `cell` now points to a known unoccupied range.
            if (nextFree.last < end_) {
                cell = nextFree.last;
                nextFree = cells_[cell].nextFree;
                continue;
            }
            prevRange->first = prevLive + blockSize_;
            memset(&cells_[prevLive + blockSize_], 0, (cell - prevLive - blockSize_) * sizeof(FixedBlockCell));
            prevRange->last = end_;
            // And we're done.
            break;
        }

        allocatedSizeTracker_.afterSweep(aliveBlocksCount * blockSize_ * sizeof(FixedBlockCell));
        liveFraction_ = end_ == 0 ? 1.0f : static_cast<float>(aliveBlocksCount * blockSize_) / static_cast<float>(end_);

        // The page is alive iff a range stored in the page header covers the entire page.
        return nextFree_.first > 0 || nextFree_.last < end_;
    }

    template <typename F>
    void TraverseAllocatedBlocks(F process) noexcept(noexcept(process(std::declval<uint8_t*>()))) {
        FixedCellRange nextFree = nextFree_; // Accessing the previous free list structure.
        for (uint32_t cell = 0 ; cell < end_ ; cell += blockSize_) {
            for (; cell < nextFree.first ; cell += blockSize_) {
                process(cells_[cell].data);
            }
            if (nextFree.last >= end_) {
                break;
            }
            cell = nextFree.last;
            nextFree = cells_[cell].nextFree;
        }
    }

    // Kore: fraction of the page occupied by objects that survived the last sweep.
    float LiveFraction() const noexcept { return liveFraction_; }

    // Kore: first and one-past-last byte this page may hold objects in.
    const uint8_t* BlocksBegin() const noexcept { return reinterpret_cast<const uint8_t*>(cells_); }
    const uint8_t* BlocksEnd() const noexcept { return reinterpret_cast<const uint8_t*>(cells_ + end_); }

    // Kore: the block containing `address`, which must be in [BlocksBegin(), BlocksEnd()).
    // Does not check whether the block is allocated.
    uint8_t* BlockContaining(const void* address) noexcept {
        auto offset = static_cast<size_t>(reinterpret_cast<const uint8_t*>(address) - BlocksBegin());
        auto cell = static_cast<uint32_t>(offset / sizeof(FixedBlockCell));
        return cells_[cell - cell % blockSize_].data;
    }

    // Testing method
    std::vector<uint8_t*> GetAllocatedBlocks() noexcept;

private:
    explicit FixedBlockPage(uint32_t blockSize) noexcept;

    FixedCellRange nextFree_;
    uint32_t blockSize_;
    uint32_t end_;
    float liveFraction_ = 0; // Kore
    // Kore: aligned for `ExtraObjectCell`s.
    alignas(32) FixedBlockCell cells_[];
};

} // namespace kotlin::alloc

#endif
