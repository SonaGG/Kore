/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#include "Nursery.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#if KONAN_WINDOWS
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "AllocationSize.hpp"
#include "CompilerConstants.hpp"
#include "GCApi.hpp"
#include "KAssert.h"
#include "Porting.h"

using namespace kotlin;

alloc::Nursery* alloc::Nursery::instance_ = nullptr;

namespace {

constexpr size_t kPinGranule = 8; // Object alignment.

uint8_t* reserve(size_t bytes) noexcept {
#if KONAN_WINDOWS
    return static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    void* memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    return memory == MAP_FAILED ? nullptr : static_cast<uint8_t*>(memory);
#endif
}

// The memory an object takes in a block: its size rounded up as allocation does (e.g. a byte array whose size is not a
// multiple of the allocation unit), so that the next object starts right after.
size_t objectBytes(uint8_t* heapObject) noexcept {
    auto size = alloc::CustomHeapObject::from(*reinterpret_cast<gc::GC::ObjectData*>(heapObject)).size();
    return alloc::AllocationSize::bytesAtLeast(size).inBytes();
}

ObjHeader* objectAt(uint8_t* heapObject) noexcept {
    return alloc::CustomHeapObject::from(*reinterpret_cast<gc::GC::ObjectData*>(heapObject)).object();
}

} // namespace

void alloc::Nursery::initialize() noexcept {
    auto mebibytes = compiler::gcNurserySize();
    // Kore: no nursery with Immix blocks (the compiler passes 0 then; this covers the runtime tests).
    if (mebibytes <= 0 || instance_ != nullptr) return;
    size_t blockCount = std::max<size_t>(static_cast<size_t>(mebibytes) * 1024 * 1024 / kBlockSize, 2);
    uint8_t* base = reserve(blockCount * kBlockSize);
    if (base == nullptr) return; // Run without a nursery.
    instance_ = new Nursery(base, blockCount);
}

alloc::Nursery::Nursery(uint8_t* base, size_t blockCount) noexcept :
    base_(base),
    blockCount_(blockCount),
    states_(new std::atomic<BlockState>[blockCount]),
    tops_(new uint32_t[blockCount]()),
    retainFlags_(new bool[blockCount]()),
    pins_(new std::atomic<uint64_t>[blockCount * kBlockSize / kPinGranule / 64]()),
    offsets_(blockCount),
    offsetsValid_(blockCount, false),
    releaseKinds_(new uint8_t[blockCount]()) {
    freeBlocks_.reserve(blockCount);
    for (size_t block = blockCount; block-- > 0;) {
        states_[block].store(BlockState::kFree, std::memory_order_relaxed);
        freeBlocks_.push_back(static_cast<uint32_t>(block));
    }
    // Fresh memory is zeroed already: blocks are cleared when they are freed.
}

bool alloc::Nursery::takeBlock(uint8_t*& start, uint8_t*& end) noexcept {
    if (!allocationEnabled()) return false;
    size_t block;
    {
        std::unique_lock guard(mutex_);
        if (freeBlocks_.empty()) return false;
        block = freeBlocks_.back();
        freeBlocks_.pop_back();
    }
    states_[block].store(BlockState::kActive, std::memory_order_relaxed);
    start = blockStart(block);
    end = start + kBlockSize;
    // Kore: debugging (`KORE_GC_VERIFY`): a released block is cleared; a write through a stale reference shows up here.
    static const bool verify = std::getenv("KORE_GC_VERIFY") != nullptr;
    if (verify) {
        auto* words = reinterpret_cast<const uintptr_t*>(start);
        for (size_t i = 0; i < kBlockSize / sizeof(uintptr_t); ++i) {
            if (words[i] != 0) {
                konan::consoleErrorf("kore-gc-verify: released nursery block %p (released by %d) written at +%zu: %p (next %p)\n",
                                     start, releaseKinds_[block], i * sizeof(uintptr_t), reinterpret_cast<void*>(words[i]),
                                     reinterpret_cast<void*>(i + 1 < kBlockSize / sizeof(uintptr_t) ? words[i + 1] : 0));
                std::abort();
            }
        }
    }
    return true;
}

void alloc::Nursery::retire(uint8_t* start, uint8_t* top) noexcept {
    size_t block = blockOf(start);
    RuntimeAssert(state(block) == BlockState::kActive, "Retiring a nursery block that is not active");
    tops_[block] = static_cast<uint32_t>(top - start);
    offsetsValid_[block] = false;
    states_[block].store(BlockState::kFull, std::memory_order_relaxed);
}

void alloc::Nursery::pin(const ObjHeader* object) noexcept {
    size_t bit = (reinterpret_cast<uintptr_t>(object) - reinterpret_cast<uintptr_t>(base_)) / kPinGranule;
    pins_[bit / 64].fetch_or(uint64_t(1) << (bit % 64), std::memory_order_relaxed);
}

bool alloc::Nursery::pinned(const ObjHeader* object) const noexcept {
    size_t bit = (reinterpret_cast<uintptr_t>(object) - reinterpret_cast<uintptr_t>(base_)) / kPinGranule;
    return (pins_[bit / 64].load(std::memory_order_relaxed) >> (bit % 64)) & 1;
}

const std::vector<uint32_t>& alloc::Nursery::objectOffsets(size_t block) noexcept {
    auto& offsets = offsets_[block];
    if (!offsetsValid_[block]) {
        offsets.clear();
        uint8_t* start = blockStart(block);
        uint32_t offset = 0;
        while (offset < tops_[block]) {
            offsets.push_back(offset);
            offset += static_cast<uint32_t>(objectBytes(start + offset));
        }
        offsetsValid_[block] = true;
    }
    return offsets;
}

ObjHeader* alloc::Nursery::objectContaining(uintptr_t address, BlockState expected) noexcept {
    if (!contains(reinterpret_cast<void*>(address))) return nullptr;
    size_t block = blockOf(reinterpret_cast<void*>(address));
    if (state(block) != expected) return nullptr;
    auto offset = static_cast<uint32_t>(address - reinterpret_cast<uintptr_t>(blockStart(block)));
    if (offset >= tops_[block]) return nullptr;
    const auto& offsets = objectOffsets(block);
    auto it = std::upper_bound(offsets.begin(), offsets.end(), offset);
    if (it == offsets.begin()) return nullptr;
    uint8_t* heapObject = blockStart(block) + *(it - 1);
    // Interior pointers (and pointers just past the end, e.g. after loop strength reduction) count.
    if (offset > *(it - 1) + objectBytes(heapObject)) return nullptr;
    return objectAt(heapObject);
}

void alloc::Nursery::forEachObject(BlockState blockState, const std::function<void(ObjHeader*)>& process) noexcept {
    for (size_t block = 0; block < blockCount_; ++block) {
        if (state(block) != blockState) continue;
        uint8_t* start = blockStart(block);
        for (uint32_t offset : objectOffsets(block)) {
            process(objectAt(start + offset));
        }
    }
}

void alloc::Nursery::retain(const ObjHeader* object) noexcept {
    retainFlags_[blockOf(object)] = true;
}

void alloc::Nursery::release(size_t block, uint8_t kind) noexcept {
    releaseKinds_[block] = kind;
    std::memset(blockStart(block), 0, tops_[block]);
    tops_[block] = 0;
    offsetsValid_[block] = false;
    retainFlags_[block] = false;
    states_[block].store(BlockState::kFree, std::memory_order_relaxed);
    std::unique_lock guard(mutex_);
    freeBlocks_.push_back(static_cast<uint32_t>(block));
}

size_t alloc::Nursery::finishYoungCollection() noexcept {
    size_t released = 0;
    for (size_t block = 0; block < blockCount_; ++block) {
        if (state(block) != BlockState::kFull) continue;
        if (retainFlags_[block]) {
            retainFlags_[block] = false;
            states_[block].store(BlockState::kRetained, std::memory_order_relaxed);
        } else {
            release(block, 1);
            ++released;
        }
    }
    for (size_t word = 0, words = blockCount_ * kBlockSize / kPinGranule / 64; word < words; ++word) {
        pins_[word].store(0, std::memory_order_relaxed);
    }
    return released;
}

size_t alloc::Nursery::releaseEmptyRetainedBlocks(const std::function<bool(ObjHeader*)>& alive) noexcept {
    size_t released = 0;
    for (size_t block = 0; block < blockCount_; ++block) {
        if (state(block) != BlockState::kRetained) continue;
        bool any = false;
        uint8_t* start = blockStart(block);
        for (uint32_t offset : objectOffsets(block)) {
            if (alive(objectAt(start + offset))) any = true;
        }
        if (!any) {
            release(block, 2);
            ++released;
        }
    }
    return released;
}
