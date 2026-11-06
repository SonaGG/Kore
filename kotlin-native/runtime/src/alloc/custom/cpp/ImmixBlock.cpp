/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#include "ImmixBlock.hpp"

#include <new>

using namespace kotlin;

alloc::ImmixBlock* alloc::ImmixBlock::Create(void* memory) noexcept {
    RuntimeAssert(reinterpret_cast<uintptr_t>(memory) % kBlockSize == 0, "Immix block at %p is not aligned", memory);
    return new (memory) ImmixBlock();
}

alloc::ImmixBlock::ImmixBlock() noexcept {
    std::memset(lines_, 0, sizeof(lines_));
    std::memset(starts_, 0, sizeof(starts_));
    // The header lines are never free.
    std::memset(lines_, 1, FirstLine());
    freeLines_ = static_cast<uint32_t>(DataLineCount());
}

size_t alloc::ImmixBlock::FirstLine() noexcept {
    return (sizeof(ImmixBlock) + kLineSize - 1) / kLineSize;
}

uint8_t* alloc::ImmixBlock::StartAtOrBefore(const void* address) noexcept {
    size_t granule = (reinterpret_cast<uintptr_t>(address) & (kBlockSize - 1)) / kGranule;
    size_t word = granule / 64;
    // Bits at or below `granule` in its word.
    uint64_t bits = starts_[word] & (~uint64_t(0) >> (63 - granule % 64));
    // Objects are at most `kMaxObjectBytes` long, but a word scan to the block start is cheap enough and also covers
    // addresses that are past the end of every object.
    while (bits == 0) {
        if (word == 0) return nullptr;
        bits = starts_[--word];
    }
    size_t bit = 63 - __builtin_clzll(bits);
    return reinterpret_cast<uint8_t*>(this) + (word * 64 + bit) * kGranule;
}

std::vector<uint8_t*> alloc::ImmixBlock::GetAllocatedBlocks() noexcept {
    std::vector<uint8_t*> allocated;
    TraverseAllocatedBlocks([&allocated](uint8_t* block) { allocated.push_back(block); });
    return allocated;
}
