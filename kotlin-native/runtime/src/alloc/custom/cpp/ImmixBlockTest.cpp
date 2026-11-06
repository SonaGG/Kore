/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#include <algorithm>
#include <cstdint>
#include <vector>

#include "CustomAllocatorTestSupport.hpp"
#include "gtest/gtest.h"

#include "ImmixBlock.hpp"
#include "ImmixBlockStore.hpp"

using namespace kotlin::alloc::test_support;

namespace {

using ImmixBlock = kotlin::alloc::ImmixBlock;
using ImmixBlockStore = kotlin::alloc::ImmixBlockStore;
using AllocatedSizeTracker = kotlin::alloc::AllocatedSizeTracker;

// Bump allocation through the holes of a block, like `CustomAllocator`.
class Bumper {
public:
    explicit Bumper(ImmixBlock* block) : block_(block), line_(ImmixBlock::FirstLine()) {}

    FakeObjectHeader* alloc(size_t bytes) {
        while (bytes > static_cast<size_t>(limit_ - cursor_)) {
            uint8_t* start;
            uint8_t* end;
            if (!block_->NextHole(line_, start, end)) return nullptr;
            if (!block_->Zeroed()) memset(start, 0, end - start);
            block_->SetUsed();
            cursor_ = start;
            limit_ = end;
        }
        uint8_t* memory = cursor_;
        cursor_ += bytes;
        EXPECT_TRUE(memory[0] == 0 && memcmp(memory, memory + 1, bytes - 1) == 0);
        ImmixBlock::MarkStart(memory);
        return new (memory) FakeObjectHeader(bytes);
    }

private:
    ImmixBlock* block_;
    size_t line_;
    uint8_t* cursor_ = nullptr;
    uint8_t* limit_ = nullptr;
};

size_t lineOf(ImmixBlock* block, const void* address) {
    return (reinterpret_cast<const uint8_t*>(address) - reinterpret_cast<const uint8_t*>(block)) / ImmixBlock::kLineSize;
}

class ImmixBlockTest : public CustomAllocatorTest {
public:
    ~ImmixBlockTest() override { store_.Clear(); }

    ImmixBlock* newBlock() { return store_.TakeFreeBlock(); }

    std::vector<std::pair<size_t, size_t>> holes(ImmixBlock* block) {
        std::vector<std::pair<size_t, size_t>> result;
        size_t line = ImmixBlock::FirstLine();
        uint8_t* start;
        uint8_t* end;
        while (block->NextHole(line, start, end)) result.emplace_back(lineOf(block, start), lineOf(block, end));
        return result;
    }

    ImmixBlockStore& store() { return store_; }

private:
    ImmixBlockStore store_;
};

} // namespace

TEST_F(ImmixBlockTest, Layout) {
    EXPECT_EQ(ImmixBlock::kLineCount, 128u);
    EXPECT_GE(ImmixBlock::FirstLine() * ImmixBlock::kLineSize, sizeof(ImmixBlock));
    EXPECT_LT((ImmixBlock::FirstLine() - 1) * ImmixBlock::kLineSize, sizeof(ImmixBlock));
    auto* block = newBlock();
    EXPECT_EQ(reinterpret_cast<uintptr_t>(block) % ImmixBlock::kBlockSize, 0u);
    EXPECT_TRUE(block->Zeroed());
    EXPECT_EQ(block->FreeLines(), ImmixBlock::DataLineCount());
    EXPECT_EQ(block->End() - block->Begin(), static_cast<ptrdiff_t>(ImmixBlock::DataLineCount() * ImmixBlock::kLineSize));
    auto expected = std::vector<std::pair<size_t, size_t>>{{ImmixBlock::FirstLine(), ImmixBlock::kLineCount}};
    EXPECT_EQ(holes(block), expected);
    EXPECT_EQ(ImmixBlock::Of(block->Begin() + 1000), block);
    EXPECT_EQ(ImmixBlock::Of(block->End() - 1), block);
}

TEST_F(ImmixBlockTest, ConsecutiveAllocation) {
    auto* block = newBlock();
    Bumper bumper(block);
    FakeObjectHeader* prev = bumper.alloc(24);
    EXPECT_EQ(reinterpret_cast<uint8_t*>(prev), block->Begin());
    size_t count = 1;
    while (auto* cur = bumper.alloc(24)) {
        EXPECT_EQ(reinterpret_cast<uint8_t*>(cur) - reinterpret_cast<uint8_t*>(prev), 24);
        prev = cur;
        ++count;
    }
    EXPECT_EQ(count, ImmixBlock::DataLineCount() * ImmixBlock::kLineSize / 24);
    EXPECT_EQ(block->GetAllocatedBlocks().size(), count);
}

TEST_F(ImmixBlockTest, SweepEmptyBlock) {
    auto* block = newBlock();
    EXPECT_EQ(block->Sweep<FakeSweepTraits>(sweepHandle(), finalizerQueue()), 0u);
    EXPECT_EQ(block->FreeLines(), ImmixBlock::DataLineCount());
}

TEST_F(ImmixBlockTest, SweepUnmarkedBlock) {
    auto* block = newBlock();
    Bumper bumper(block);
    while (bumper.alloc(40)) {
    }
    EXPECT_EQ(block->Sweep<FakeSweepTraits>(sweepHandle(), finalizerQueue()), 0u);
    EXPECT_EQ(block->FreeLines(), ImmixBlock::DataLineCount());
    EXPECT_TRUE(block->GetAllocatedBlocks().empty());
    auto expected = std::vector<std::pair<size_t, size_t>>{{ImmixBlock::FirstLine(), ImmixBlock::kLineCount}};
    EXPECT_EQ(holes(block), expected);
}

TEST_F(ImmixBlockTest, SweepKeepsMarkedObjectsAndTheirLines) {
    auto* block = newBlock();
    Bumper bumper(block);
    std::vector<FakeObjectHeader*> objects;
    while (auto* object = bumper.alloc(64)) objects.push_back(object);
    // Keep the objects of every fourth line (4 objects per line).
    std::vector<FakeObjectHeader*> kept;
    for (auto* object : objects) {
        if (lineOf(block, object) % 4 == 0) {
            object->mark();
            kept.push_back(object);
        }
    }
    size_t live = block->Sweep<FakeSweepTraits>(sweepHandle(), finalizerQueue());
    EXPECT_EQ(live, kept.size() * 64);
    std::vector<uint8_t*> expectedObjects;
    for (auto* object : kept) expectedObjects.push_back(reinterpret_cast<uint8_t*>(object));
    EXPECT_EQ(block->GetAllocatedBlocks(), expectedObjects);
    size_t usedLines = 0;
    for (size_t line = ImmixBlock::FirstLine(); line < ImmixBlock::kLineCount; ++line) {
        EXPECT_EQ(block->LineUsed(line), line % 4 == 0) << line;
        usedLines += line % 4 == 0;
    }
    EXPECT_EQ(block->FreeLines(), ImmixBlock::DataLineCount() - usedLines);
    for (auto [first, end] : holes(block)) {
        EXPECT_NE(first % 4, 0u);
        EXPECT_TRUE(end == ImmixBlock::kLineCount || end % 4 == 0);
    }

    // Allocation fills the holes only, with zeroed memory.
    Bumper refill(block);
    while (auto* object = refill.alloc(64)) {
        EXPECT_NE(lineOf(block, object) % 4, 0u);
    }
    EXPECT_EQ(block->GetAllocatedBlocks().size(), objects.size());
    for (auto* object : kept) EXPECT_EQ(object->getSize(), 64u);
}

TEST_F(ImmixBlockTest, SweepMarksAllLinesOfAnObject) {
    auto* block = newBlock();
    Bumper bumper(block);
    auto* small = bumper.alloc(16);
    auto* medium = bumper.alloc(3 * ImmixBlock::kLineSize);
    auto* last = bumper.alloc(16);
    medium->mark();
    EXPECT_EQ(block->Sweep<FakeSweepTraits>(sweepHandle(), finalizerQueue()), 3 * ImmixBlock::kLineSize);
    size_t first = lineOf(block, medium);
    size_t end = lineOf(block, reinterpret_cast<uint8_t*>(medium) + 3 * ImmixBlock::kLineSize - 1) + 1;
    EXPECT_EQ(end - first, 4u); // Starts 16 bytes into a line.
    for (size_t line = ImmixBlock::FirstLine(); line < ImmixBlock::kLineCount; ++line) {
        EXPECT_EQ(block->LineUsed(line), line >= first && line < end) << line;
    }
    EXPECT_FALSE(block->IsStart(small));
    EXPECT_TRUE(block->IsStart(medium));
    EXPECT_FALSE(block->IsStart(last));
}

TEST_F(ImmixBlockTest, StartAtOrBefore) {
    auto* block = newBlock();
    EXPECT_EQ(block->StartAtOrBefore(block->Begin() + 100), nullptr);
    EXPECT_EQ(block->StartAtOrBefore(block), nullptr);
    Bumper bumper(block);
    std::vector<FakeObjectHeader*> objects;
    for (size_t bytes : {16u, 24u, 1000u, 32u, 4096u, 520u}) objects.push_back(bumper.alloc(bytes));
    for (auto* object : objects) {
        auto* start = reinterpret_cast<uint8_t*>(object);
        for (size_t offset = 0; offset < object->getSize(); offset += 8) {
            EXPECT_EQ(block->StartAtOrBefore(start + offset), start);
            EXPECT_EQ(block->StartAtOrBefore(start + offset + 7), start);
        }
    }
    // Past the last object: still the last start, the caller checks the bounds.
    auto* lastStart = reinterpret_cast<uint8_t*>(objects.back());
    EXPECT_EQ(block->StartAtOrBefore(block->End() - 1), lastStart);
    // Before the first object (in the header lines).
    EXPECT_EQ(block->StartAtOrBefore(block->Begin() - 8), nullptr);
}

TEST_F(ImmixBlockTest, StoreSweepClassifiesBlocksAndTracksBytes) {
    AllocatedSizeTracker::Heap tracker;
    auto* empty = store().TakeBlock();
    auto* sparse = store().TakeBlock();
    auto* dense = store().TakeBlock();
    EXPECT_NE(empty, sparse);
    EXPECT_NE(sparse, dense);
    size_t taken = 0;
    for (auto* block : {empty, sparse, dense}) {
        size_t bytes = block->End() - block->Begin();
        block->RecordedBytes() += bytes;
        tracker.recordDifference(static_cast<std::ptrdiff_t>(bytes));
        taken += bytes;
    }
    EXPECT_EQ(tracker.allocatedBytes(), taken);
    {
        Bumper bumper(empty);
        while (bumper.alloc(32)) {
        }
    }
    {
        Bumper bumper(sparse);
        bumper.alloc(32)->mark();
        while (bumper.alloc(32)) {
        }
    }
    size_t denseLive = 0;
    {
        Bumper bumper(dense);
        while (auto* object = bumper.alloc(32)) {
            object->mark();
            denseLive += 32;
        }
    }

    store().PrepareForYoungGC();
    store().Sweep<FakeSweepTraits>(sweepHandle(), finalizerQueue(), tracker);
    EXPECT_EQ(tracker.allocatedBytes(), 32 + denseLive);
    EXPECT_EQ(empty->RecordedBytes(), 0u);
    EXPECT_EQ(sparse->RecordedBytes(), 32u);
    EXPECT_EQ(dense->FreeLines(), 0u);

    // The sparse block is recycled first, then free blocks (the empty one among them); the dense one is not reused.
    EXPECT_EQ(store().TakeBlock(), sparse);
    std::vector<ImmixBlock*> taken2;
    for (size_t i = 0; i < 2 * ImmixBlockStore::kBlocksPerChunk; ++i) taken2.push_back(store().TakeBlock());
    EXPECT_NE(std::find(taken2.begin(), taken2.end(), empty), taken2.end());
    EXPECT_EQ(std::find(taken2.begin(), taken2.end(), dense), taken2.end());

    // A full collection sweeps the dense block again.
    for (auto* object : dense->GetAllocatedBlocks()) FakeObjectHeader::at(object)->mark();
    store().PrepareForGC();
    store().Sweep<FakeSweepTraits>(sweepHandle(), finalizerQueue(), tracker);
    EXPECT_EQ(tracker.allocatedBytes(), denseLive);
}

TEST_F(ImmixBlockTest, StoreBlockContaining) {
    auto* block = store().TakeBlock();
    EXPECT_EQ(store().BlockContaining(block->Begin() + 123), block);
    EXPECT_EQ(store().BlockContaining(block), block);
    int local = 0;
    EXPECT_EQ(store().BlockContaining(&local), nullptr);
    size_t blocks = 0;
    store().TraverseBlocks([&](ImmixBlock*) { ++blocks; });
    EXPECT_EQ(blocks, ImmixBlockStore::kBlocksPerChunk);
}
