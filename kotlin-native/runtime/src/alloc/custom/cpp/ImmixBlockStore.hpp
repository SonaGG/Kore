/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "AllocatedSizeTracker.hpp"
#include "AllocationSize.hpp"
#include "AtomicStack.hpp"
#include "CustomFinalizerProcessor.hpp"
#include "ImmixBlock.hpp"

namespace kotlin::alloc {

// Kore: the Immix blocks of the heap (memory v2), like `PageStore` for pages.
//
// Blocks come from chunks of `kBlocksPerChunk` blocks, which are kept for the lifetime of the heap (free blocks are
// reused, not returned to the system), so that conservative references can be resolved by a chunk lookup and a mask.
// Every block is in exactly one list:
//   free_        no live objects.
//   recyclable_  swept, with at least `kMinFreeLinesToRecycle` free lines, not allocated into since.
//   full_        swept, too full to allocate into; old objects only (until the next full collection sweeps it).
//   used_        handed to a thread for allocation since the last collection.
//   unswept_     to be swept by the collection in progress.
// A young collection sweeps only `used_`: the other blocks received no objects since the previous collection, so all
// their objects are old under the sticky-mark generational GC.
class ImmixBlockStore {
public:
    static constexpr size_t kBlocksPerChunk = 32;
    // Recycling a mostly full block yields small holes for the cost of sweeping its old objects in every young
    // collection until the next full one.
    static constexpr size_t kMinFreeLinesToRecycle = 16;

    ImmixBlockStore() noexcept = default;
    ~ImmixBlockStore() noexcept { Clear(); }

    void PrepareForGC() noexcept {
        unswept_.TransferAllFrom(std::move(used_));
        unswept_.TransferAllFrom(std::move(recyclable_));
        unswept_.TransferAllFrom(std::move(full_));
    }

    void PrepareForYoungGC() noexcept { unswept_.TransferAllFrom(std::move(used_)); }

    // Sweeps blocks until none is left. Several threads may sweep at once.
    template <typename SweepTraits>
    void Sweep(typename SweepTraits::GCSweepScope& sweepHandle, FinalizerQueue& finalizerQueue, AllocatedSizeTracker::Heap& tracker) noexcept {
        std::ptrdiff_t diff = 0;
        while (ImmixBlock* block = unswept_.Pop()) {
            size_t liveBytes = block->Sweep<SweepTraits>(sweepHandle, finalizerQueue);
            diff += static_cast<std::ptrdiff_t>(liveBytes) - static_cast<std::ptrdiff_t>(block->RecordedBytes());
            block->RecordedBytes() = liveBytes;
            if (liveBytes == 0) {
                free_.Push(block);
            } else if (block->FreeLines() >= kMinFreeLinesToRecycle) {
                recyclable_.Push(block);
            } else {
                full_.Push(block);
            }
        }
        if (diff != 0) tracker.recordDifference(diff);
    }

    // A block to allocate into: recyclable if there is one, else free or new.
    ImmixBlock* TakeBlock() noexcept {
        ImmixBlock* block = recyclable_.Pop();
        if (block == nullptr) return TakeFreeBlock();
        used_.Push(block);
        return block;
    }

    // A block with no objects (for medium objects that do not fit the holes of recycled blocks).
    ImmixBlock* TakeFreeBlock() noexcept {
        ImmixBlock* block = free_.Pop();
        if (block == nullptr) block = NewBlock();
        used_.Push(block);
        return block;
    }

    // The block containing `address` if it is in one of the chunks, else null. The chunks must not change meanwhile
    // (the world is stopped, or allocation is otherwise excluded).
    ImmixBlock* BlockContaining(const void* address) noexcept;

    // Calls `process(begin, end)` for the memory ranges of all chunks, in ascending order.
    template <typename F>
    void TraverseChunks(F process) {
        std::unique_lock guard(chunksMutex_);
        for (auto& chunk : chunks_) process(chunk.blocks, chunk.blocks + kBlocksPerChunk * ImmixBlock::kBlockSize);
    }

    template <typename F>
    void TraverseBlocks(F process) {
        std::unique_lock guard(chunksMutex_);
        for (auto& chunk : chunks_) {
            for (size_t i = 0; i < kBlocksPerChunk; ++i) {
                process(reinterpret_cast<ImmixBlock*>(chunk.blocks + i * ImmixBlock::kBlockSize));
            }
        }
    }

    void Clear() noexcept;

private:
    struct Chunk {
        void* memory;
        uint8_t* blocks; // Aligned to `ImmixBlock::kBlockSize`.
    };

    ImmixBlock* NewBlock() noexcept;

    AtomicStack<ImmixBlock> free_;
    AtomicStack<ImmixBlock> recyclable_;
    AtomicStack<ImmixBlock> full_;
    AtomicStack<ImmixBlock> used_;
    AtomicStack<ImmixBlock> unswept_;

    std::mutex chunksMutex_;
    std::vector<Chunk> chunks_; // Sorted by address.
};

} // namespace kotlin::alloc
