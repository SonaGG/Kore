/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#include "ImmixBlockStore.hpp"

#include <algorithm>

#include "CustomLogging.hpp"
#include "GCApi.hpp"

using namespace kotlin;

namespace {

constexpr size_t kChunkBytes = alloc::ImmixBlockStore::kBlocksPerChunk * alloc::ImmixBlock::kBlockSize;
// One more block to align the blocks.
constexpr size_t kChunkAllocationBytes = kChunkBytes + alloc::ImmixBlock::kBlockSize;

} // namespace

alloc::ImmixBlock* alloc::ImmixBlockStore::NewBlock() noexcept {
    // Fresh memory is zeroed (see `SafeAlloc`).
    void* memory = SafeAlloc(kChunkAllocationBytes);
    auto address = reinterpret_cast<uintptr_t>(memory);
    auto* blocks = reinterpret_cast<uint8_t*>((address + ImmixBlock::kBlockSize - 1) & ~(uintptr_t(ImmixBlock::kBlockSize) - 1));
    CustomAllocInfo("ImmixBlockStore: new chunk at %p", blocks);
    {
        std::unique_lock guard(chunksMutex_);
        Chunk chunk{memory, blocks};
        auto position = std::upper_bound(
                chunks_.begin(), chunks_.end(), chunk, [](const Chunk& lhs, const Chunk& rhs) { return lhs.blocks < rhs.blocks; });
        chunks_.insert(position, chunk);
    }
    for (size_t i = 1; i < kBlocksPerChunk; ++i) {
        free_.Push(ImmixBlock::Create(blocks + i * ImmixBlock::kBlockSize));
    }
    return ImmixBlock::Create(blocks);
}

alloc::ImmixBlock* alloc::ImmixBlockStore::BlockContaining(const void* address) noexcept {
    auto* byte = reinterpret_cast<const uint8_t*>(address);
    std::unique_lock guard(chunksMutex_);
    auto position = std::upper_bound(
            chunks_.begin(), chunks_.end(), byte, [](const uint8_t* value, const Chunk& chunk) { return value < chunk.blocks; });
    if (position == chunks_.begin()) return nullptr;
    --position;
    if (byte >= position->blocks + kChunkBytes) return nullptr;
    return ImmixBlock::Of(address);
}

void alloc::ImmixBlockStore::Clear() noexcept {
    while (free_.Pop()) {}
    while (recyclable_.Pop()) {}
    while (full_.Pop()) {}
    while (used_.Pop()) {}
    while (unswept_.Pop()) {}
    std::unique_lock guard(chunksMutex_);
    for (auto& chunk : chunks_) Free(chunk.memory, kChunkAllocationBytes);
    chunks_.clear();
}
