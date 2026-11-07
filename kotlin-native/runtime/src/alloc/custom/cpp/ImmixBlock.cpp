/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#include "ImmixBlock.hpp"

#include <cstdlib>

#include "Porting.h"

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

void alloc::ImmixBlock::EnsureNurseryStarts() noexcept {
    if (!nursery_ || nurseryStartsBuilt_.load(std::memory_order_acquire)) return;
    static std::mutex mutex;
    std::unique_lock guard(mutex);
    if (nurseryStartsBuilt_.load(std::memory_order_relaxed)) return;
    auto* base = reinterpret_cast<uint8_t*>(this);
    uint8_t* end = base + nurseryTop_;
    for (uint8_t* element = Begin(); element < end;) {
        // A zero word: nothing was allocated from here on (cannot happen below the top, but stay safe).
        if (*reinterpret_cast<const uintptr_t*>(element) == 0) break;
        MarkStart(element);
        element += nurseryElementBytes(element);
    }
    nurseryStartsBuilt_.store(true, std::memory_order_release);
}

bool alloc::immixCheck() noexcept {
    static const bool check = std::getenv("KORE_IMMIX_CHECK") != nullptr;
    return check;
}

namespace {
void dumpBlock(const alloc::ImmixBlock* block, const uint8_t* lines, const uint64_t* starts, size_t lineCount, size_t startWords) noexcept {
    konan::consoleErrorf("kore-immix: block %p lines:", block);
    for (size_t i = 0; i < lineCount; ++i) konan::consoleErrorf("%c", lines[i] ? '#' : '.');
    konan::consoleErrorf("\nkore-immix: starts:");
    for (size_t w = 0; w < startWords; ++w) if (starts[w] != 0) konan::consoleErrorf(" [%zu]=%016llx", w, static_cast<unsigned long long>(starts[w]));
    konan::consoleErrorf("\n");
}
} // namespace

[[noreturn]] void alloc::reportZeroHeaderStart(const uint8_t* element) noexcept {
    auto* block = ImmixBlock::Of(element);
    konan::consoleErrorf("kore-immix: sweep found an object start at %p (block %p +%zu) with a zero header (thread %zu)\n", element,
                         block, static_cast<size_t>(element - reinterpret_cast<const uint8_t*>(block)), static_cast<size_t>(konan::currentThreadId()));
    block->CheckNoStartsIn(nullptr, nullptr, "dump");
    std::abort();
}

void alloc::ImmixBlock::CheckNoStartsIn(const uint8_t* start, const uint8_t* end, const char* what) noexcept {
    auto* base = reinterpret_cast<const uint8_t*>(this);
    if (start == nullptr) { dumpBlock(this, lines_, starts_, kLineCount, kStartWords); return; }
    for (const uint8_t* p = start; p < end; p += kGranule) {
        if (IsStart(p)) {
            konan::consoleErrorf("kore-immix: %s: object start at %p (+%zu) inside the hole [+%zu, +%zu) of block %p (recycled %d nursery %d zeroed %d, thread %zu)\n",
                                 what, p, static_cast<size_t>(p - base), static_cast<size_t>(start - base), static_cast<size_t>(end - base), this,
                                 recycled_, nursery_, zeroed_, static_cast<size_t>(konan::currentThreadId()));
            dumpBlock(this, lines_, starts_, kLineCount, kStartWords);
            std::abort();
        }
    }
}

std::vector<uint8_t*> alloc::ImmixBlock::GetAllocatedBlocks() noexcept {
    std::vector<uint8_t*> allocated;
    TraverseAllocatedBlocks([&allocated](uint8_t* block) { allocated.push_back(block); });
    return allocated;
}
