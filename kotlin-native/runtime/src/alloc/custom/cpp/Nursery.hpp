/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "Memory.h"
#include "Utils.hpp"

namespace kotlin::alloc {

// Kore: the moving young generation (see `alloc::nursery` in Allocator.hpp).
//
// One contiguous reservation split into blocks. A thread allocates small objects by bumping a pointer through a block
// it owns (`CustomAllocator`); objects are laid out back to back, so the objects of a block can be walked from its
// start, which maps any address in it to its object. A young collection copies the live objects into the regular heap,
// except pinned ones, whose blocks are retained: they then hold old objects, swept by full collections.
class Nursery : private Pinned {
public:
    static constexpr size_t kBlockSize = 256 * 1024;
    static constexpr size_t kMaxObjectBytes = 4 * 1024;

    enum class BlockState : uint8_t {
        kFree,
        kActive, // Owned by a thread for allocation.
        kFull, // Retired: holds young objects up to `top`.
        kRetained, // Holds old objects promoted in place, up to `top`.
    };

    // The nursery, or null if there is none.
    static Nursery* instance() noexcept { return instance_; }
    static void initialize() noexcept;

    bool allocationEnabled() const noexcept { return allocationEnabled_.load(std::memory_order_relaxed); }
    void setAllocationEnabled(bool enabled) noexcept { allocationEnabled_.store(enabled, std::memory_order_relaxed); }

    // A zeroed block for allocation, as `[start, end)`; false if none is free.
    bool takeBlock(uint8_t*& start, uint8_t*& end) noexcept;
    // Ends allocation in the block starting at `start`, whose objects end at `top`.
    void retire(uint8_t* start, uint8_t* top) noexcept;

    bool contains(const void* address) const noexcept {
        return reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(base_) < blockCount_ * kBlockSize;
    }
    size_t blockOf(const void* address) const noexcept {
        return (reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(base_)) / kBlockSize;
    }
    BlockState state(size_t block) const noexcept { return states_[block].load(std::memory_order_relaxed); }
    size_t countBlocks(BlockState blockState) const noexcept {
        size_t count = 0;
        for (size_t block = 0; block < blockCount_; ++block) count += state(block) == blockState;
        return count;
    }
    uint8_t releaseKind(const void* address) const noexcept { return contains(address) ? releaseKinds_[blockOf(address)] : 0; }
    bool movable(const void* address) const noexcept {
        if (!contains(address)) return false;
        auto blockState = state(blockOf(address));
        return blockState == BlockState::kFull || blockState == BlockState::kActive;
    }

    void pin(const ObjHeader* object) noexcept;
    bool pinned(const ObjHeader* object) const noexcept;

    // The object of a `kFull` or `kRetained` block whose memory contains `address`, if any.
    ObjHeader* objectContaining(uintptr_t address, BlockState expected) noexcept;
    void forEachObject(BlockState blockState, const std::function<void(ObjHeader*)>& process) noexcept;

    void retain(const ObjHeader* object) noexcept;
    // Whether the block of `object` is kept by the young collection in progress.
    bool retaining(const void* address) const noexcept { return contains(address) && retainFlags_[blockOf(address)]; }
    // Both return the number of blocks they free.
    size_t finishYoungCollection() noexcept;
    size_t releaseEmptyRetainedBlocks(const std::function<bool(ObjHeader*)>& alive) noexcept;

private:
    Nursery(uint8_t* base, size_t blockCount) noexcept;

    uint8_t* blockStart(size_t block) const noexcept { return base_ + block * kBlockSize; }
    const std::vector<uint32_t>& objectOffsets(size_t block) noexcept;
    void release(size_t block, uint8_t kind) noexcept;

    static Nursery* instance_;

    uint8_t* const base_;
    const size_t blockCount_;
    std::unique_ptr<std::atomic<BlockState>[]> states_;
    std::unique_ptr<uint32_t[]> tops_; // End of the objects of `kFull` and `kRetained` blocks.
    std::unique_ptr<bool[]> retainFlags_; // Set during a young collection.
    std::unique_ptr<std::atomic<uint64_t>[]> pins_; // One bit per object alignment unit.
    std::vector<std::vector<uint32_t>> offsets_; // Object start offsets per block, built on demand.
    std::vector<bool> offsetsValid_;
    std::unique_ptr<uint8_t[]> releaseKinds_; // Debugging: how each block was last released (1 young, 2 retained).
    std::atomic<bool> allocationEnabled_ = true;
    std::mutex mutex_;
    std::vector<uint32_t> freeBlocks_;
};

} // namespace kotlin::alloc
