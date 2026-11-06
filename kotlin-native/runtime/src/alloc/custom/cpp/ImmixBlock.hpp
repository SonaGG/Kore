/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "AnyPage.hpp"
#include "AllocationSize.hpp"
#include "Constants.hpp"
#include "CustomFinalizerProcessor.hpp"
#include "KAssert.h"

namespace kotlin::alloc {

// Kore: memory v2 (docs/kore/memory-v2.md). A 32 KiB block of 256-byte lines for small and medium objects (Immix,
// Blackburn & McKinley, PLDI 2008). Blocks are aligned to their size, so any address maps to its block by masking.
//
// The block header occupies the first lines: a line byte map (0: free) and an object-start bitmap (one bit per 8-byte
// granule, set when an object is allocated). Threads allocate by bumping a cursor through holes (runs of free lines,
// zeroed when taken). The sweep walks the start bitmap: dead objects lose their start bit, live objects mark the lines
// they cover, and the free lines become the holes of the next allocations. Objects never move.
class alignas(kPageAlignment) ImmixBlock : public AnyPage<ImmixBlock> {
public:
    static constexpr size_t kBlockSize = 32 * KiB;
    static constexpr size_t kLineSize = 256;
    static constexpr size_t kLineCount = kBlockSize / kLineSize;
    static constexpr size_t kGranule = 8;
    static constexpr size_t kGranuleCount = kBlockSize / kGranule;
    static constexpr size_t kStartWords = kGranuleCount / 64;
    // Larger objects get their own `SingleObjectPage`.
    static constexpr size_t kMaxObjectBytes = 8 * KiB;

    // Initializes a block in zeroed, `kBlockSize`-aligned memory.
    static ImmixBlock* Create(void* memory) noexcept;

    // The block of an address inside it.
    static ImmixBlock* Of(const void* address) noexcept {
        return reinterpret_cast<ImmixBlock*>(reinterpret_cast<uintptr_t>(address) & ~(uintptr_t(kBlockSize) - 1));
    }

    static size_t FirstLine() noexcept;

    uint8_t* Begin() noexcept { return reinterpret_cast<uint8_t*>(this) + FirstLine() * kLineSize; }
    uint8_t* End() noexcept { return reinterpret_cast<uint8_t*>(this) + kBlockSize; }
    static size_t DataLineCount() noexcept { return kLineCount - FirstLine(); }

    // Records an object allocated at `address` (8-byte aligned, inside a hole of its block).
    ALWAYS_INLINE static void MarkStart(uint8_t* address) noexcept {
        auto* block = Of(address);
        size_t granule = (reinterpret_cast<uintptr_t>(address) & (kBlockSize - 1)) / kGranule;
        block->starts_[granule / 64] |= uint64_t(1) << (granule % 64);
    }

    bool IsStart(const void* address) const noexcept {
        size_t granule = (reinterpret_cast<uintptr_t>(address) & (kBlockSize - 1)) / kGranule;
        return (starts_[granule / 64] & (uint64_t(1) << (granule % 64))) != 0;
    }

    // The closest object start at or before `address` (in this block), or null. The object need not contain `address`.
    uint8_t* StartAtOrBefore(const void* address) noexcept;

    // The next hole at or after line `line`: [`start`, `end`). Advances `line` past it. False if there is none.
    bool NextHole(size_t& line, uint8_t*& start, uint8_t*& end) noexcept {
        while (line < kLineCount && lines_[line] != 0) ++line;
        if (line >= kLineCount) return false;
        size_t first = line;
        while (line < kLineCount && lines_[line] == 0) ++line;
        start = reinterpret_cast<uint8_t*>(this) + first * kLineSize;
        end = reinterpret_cast<uint8_t*>(this) + line * kLineSize;
        return true;
    }

    // Whether the free memory of the block is known to be zeroed (the block was never allocated into).
    bool Zeroed() const noexcept { return zeroed_; }
    void SetUsed() noexcept { zeroed_ = false; }

    size_t FreeLines() const noexcept { return freeLines_; }
    bool LineUsed(size_t line) const noexcept { return lines_[line] != 0; }

    // Bytes reported to the heap's `AllocatedSizeTracker` for this block: the live bytes after the last sweep plus the
    // holes taken since.
    size_t& RecordedBytes() noexcept { return recordedBytes_; }

    // Sweeps the objects of the block. Returns the bytes of the live objects. Must not run concurrently with
    // allocation in this block.
    template <typename SweepTraits>
    size_t Sweep(typename SweepTraits::GCSweepScope& sweepHandle, FinalizerQueue& finalizerQueue) noexcept {
        size_t firstLine = FirstLine();
        std::memset(lines_ + firstLine, 0, kLineCount - firstLine);
        size_t liveBytes = 0;
        auto* base = reinterpret_cast<uint8_t*>(this);
        for (size_t word = 0; word < kStartWords; ++word) {
            uint64_t bits = starts_[word];
            uint64_t remaining = bits;
            while (remaining != 0) {
                size_t bit = __builtin_ctzll(remaining);
                remaining &= remaining - 1;
                size_t offset = (word * 64 + bit) * kGranule;
                uint8_t* element = base + offset;
                if (SweepTraits::trySweepElement(element, finalizerQueue, sweepHandle)) {
                    bits &= ~(uint64_t(1) << bit);
                    continue;
                }
                size_t bytes = SweepTraits::elementSize(element).inBytes();
                liveBytes += bytes;
                // Live objects mark exactly the lines they cover (their sizes are known, so no conservative marking).
                size_t first = offset / kLineSize;
                size_t last = (offset + bytes - 1) / kLineSize;
                RuntimeAssert(last < kLineCount, "Object at %p of %zu bytes crosses its block", element, bytes);
                std::memset(lines_ + first, 1, last - first + 1);
            }
            starts_[word] = bits;
        }
        size_t freeLines = 0;
        for (size_t line = firstLine; line < kLineCount; ++line) freeLines += lines_[line] == 0;
        freeLines_ = static_cast<uint32_t>(freeLines);
        return liveBytes;
    }

    template <typename F>
    void TraverseAllocatedBlocks(F process) noexcept(noexcept(process(std::declval<uint8_t*>()))) {
        auto* base = reinterpret_cast<uint8_t*>(this);
        for (size_t word = 0; word < kStartWords; ++word) {
            uint64_t bits = starts_[word];
            while (bits != 0) {
                size_t bit = __builtin_ctzll(bits);
                bits &= bits - 1;
                process(base + (word * 64 + bit) * kGranule);
            }
        }
    }

    // Testing method
    std::vector<uint8_t*> GetAllocatedBlocks() noexcept;

private:
    ImmixBlock() noexcept;

    size_t recordedBytes_ = 0;
    uint32_t freeLines_ = 0;
    bool zeroed_ = true;
    uint8_t lines_[kLineCount];
    uint64_t starts_[kStartWords];
};

static_assert(ImmixBlock::kMaxObjectBytes < ImmixBlock::kBlockSize / 2, "Medium objects must fit an empty block");

} // namespace kotlin::alloc
