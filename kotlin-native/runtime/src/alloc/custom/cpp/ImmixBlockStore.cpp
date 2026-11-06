/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#include "ImmixBlockStore.hpp"

#include <algorithm>

#include "CustomLogging.hpp"
#include "GCApi.hpp"
#include "HeapReservation.hpp"

using namespace kotlin;

namespace {

constexpr size_t kChunkBytes = alloc::ImmixBlockStore::kBlocksPerChunk * alloc::ImmixBlock::kBlockSize;
// Without the heap reservation: one more block to align the blocks.
constexpr size_t kUnalignedChunkAllocationBytes = kChunkBytes + alloc::ImmixBlock::kBlockSize;
// Free blocks kept committed (beyond the share of the blocks in use below): a young generation's worth of allocation.
constexpr size_t kMinRetainedFreeBlocks = 16 * 1024 * 1024 / alloc::ImmixBlock::kBlockSize;

} // namespace

alloc::ImmixBlock* alloc::ImmixBlockStore::NewBlock() noexcept {
    // Fresh memory is zeroed (see `SafeAlloc`).
    size_t memoryBytes = kChunkBytes;
    void* memory = SafeAllocAligned(kChunkBytes, ImmixBlock::kBlockSize);
    if (memory == nullptr) {
        memoryBytes = kUnalignedChunkAllocationBytes;
        memory = SafeAlloc(memoryBytes);
    }
    auto address = reinterpret_cast<uintptr_t>(memory);
    auto* blocks = reinterpret_cast<uint8_t*>((address + ImmixBlock::kBlockSize - 1) & ~(uintptr_t(ImmixBlock::kBlockSize) - 1));
    CustomAllocInfo("ImmixBlockStore: new chunk at %p", blocks);
    {
        std::unique_lock guard(chunksMutex_);
        Chunk chunk{memory, memoryBytes, blocks, 0};
        auto position = std::upper_bound(
                chunks_.begin(), chunks_.end(), chunk, [](const Chunk& lhs, const Chunk& rhs) { return lhs.blocks < rhs.blocks; });
        chunks_.insert(position, chunk);
    }
    for (size_t i = 1; i < kBlocksPerChunk; ++i) {
        PushFree(ImmixBlock::Create(blocks + i * ImmixBlock::kBlockSize));
    }
    return ImmixBlock::Create(blocks);
}

alloc::ImmixBlockStore::Chunk* alloc::ImmixBlockStore::ChunkContaining(const void* address) noexcept {
    auto* byte = reinterpret_cast<const uint8_t*>(address);
    auto position = std::upper_bound(
            chunks_.begin(), chunks_.end(), byte, [](const uint8_t* value, const Chunk& chunk) { return value < chunk.blocks; });
    if (position == chunks_.begin()) return nullptr;
    --position;
    if (byte >= position->blocks + kChunkBytes) return nullptr;
    return &*position;
}

alloc::ImmixBlock* alloc::ImmixBlockStore::BlockContaining(const void* address) noexcept {
    std::unique_lock guard(chunksMutex_);
    Chunk* chunk = ChunkContaining(address);
    if (chunk == nullptr) return nullptr;
    size_t index = (reinterpret_cast<const uint8_t*>(address) - chunk->blocks) / ImmixBlock::kBlockSize;
    if ((chunk->decommitted & (uint32_t(1) << index)) != 0) return nullptr;
    return ImmixBlock::Of(address);
}

void alloc::ImmixBlockStore::SelectBlocksToRelease() noexcept {
    auto* reservation = HeapReservation::instance();
    if (reservation == nullptr) return;
    size_t totalBlocks;
    {
        std::unique_lock guard(chunksMutex_);
        totalBlocks = chunks_.size() * kBlocksPerChunk;
    }
    size_t decommittedBlocks;
    {
        std::unique_lock guard(decommittedMutex_);
        decommittedBlocks = decommitted_.size();
    }
    size_t freeBlocks = freeBlocks_.load(std::memory_order_relaxed);
    // The counts are not taken atomically together; clamp.
    size_t notUsed = decommittedBlocks + toRelease_.size() + freeBlocks;
    size_t usedBlocks = totalBlocks > notUsed ? totalBlocks - notUsed : 0;
    // Keep a quarter of the blocks in use free, for allocation to come.
    size_t retained = std::max(kMinRetainedFreeBlocks, usedBlocks / 4);
    while (freeBlocks > retained) {
        ImmixBlock* block = free_.Pop();
        if (block == nullptr) break;
        freeBlocks_.fetch_sub(1, std::memory_order_relaxed);
        --freeBlocks;
        toRelease_.push_back(block);
    }
}

void alloc::ImmixBlockStore::ReleaseBlocks() noexcept {
    if (toRelease_.empty()) return;
    auto* reservation = HeapReservation::instance();
    for (ImmixBlock* block : toRelease_) {
        {
            // Marked first: from then on, nobody reads the block (`BlockContaining`, `TraverseBlocks`).
            std::unique_lock guard(chunksMutex_);
            Chunk* chunk = ChunkContaining(block);
            size_t index = (reinterpret_cast<uint8_t*>(block) - chunk->blocks) / ImmixBlock::kBlockSize;
            chunk->decommitted |= uint32_t(1) << index;
        }
        reservation->decommit(block, ImmixBlock::kBlockSize);
    }
    CustomAllocInfo("ImmixBlockStore: returned %zu blocks to the system", toRelease_.size());
    std::unique_lock guard(decommittedMutex_);
    decommitted_.insert(decommitted_.end(), toRelease_.begin(), toRelease_.end());
    toRelease_.clear();
}

alloc::ImmixBlock* alloc::ImmixBlockStore::TakeDecommittedBlock() noexcept {
    void* memory;
    {
        std::unique_lock guard(decommittedMutex_);
        if (decommitted_.empty()) return nullptr;
        memory = decommitted_.back();
        decommitted_.pop_back();
    }
    // Committed memory is zeroed.
    HeapReservation::instance()->commit(memory, ImmixBlock::kBlockSize);
    ImmixBlock* block = ImmixBlock::Create(memory);
    std::unique_lock guard(chunksMutex_);
    Chunk* chunk = ChunkContaining(memory);
    size_t index = (static_cast<uint8_t*>(memory) - chunk->blocks) / ImmixBlock::kBlockSize;
    chunk->decommitted &= ~(uint32_t(1) << index);
    return block;
}

void alloc::ImmixBlockStore::Clear() noexcept {
    while (free_.Pop()) {}
    while (recyclable_.Pop()) {}
    while (full_.Pop()) {}
    while (used_.Pop()) {}
    while (unswept_.Pop()) {}
    freeBlocks_.store(0, std::memory_order_relaxed);
    toRelease_.clear();
    {
        std::unique_lock guard(decommittedMutex_);
        decommitted_.clear();
    }
    std::unique_lock guard(chunksMutex_);
    for (auto& chunk : chunks_) {
        if (chunk.decommitted != 0) HeapReservation::instance()->commit(chunk.blocks, kChunkBytes);
        Free(chunk.memory, chunk.memoryBytes);
    }
    chunks_.clear();
}
