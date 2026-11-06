/*
 * Copyright 2022 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#include "Heap.hpp"
#include "PageRegistry.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <cinttypes>
#include <new>
#include <vector>

#include "AtomicStack.hpp"
#include "CustomLogging.hpp"
#include "ExtraObjectData.hpp"
#include "GCApi.hpp"
#include "Memory.h"
#include "ThreadRegistry.hpp"

namespace kotlin::gc {
void pausePhase(const char* name) noexcept;
} // namespace kotlin::gc

namespace kotlin::alloc {

void Heap::PrepareForGC() noexcept {
    CustomAllocDebug("Heap::PrepareForGC()");
    nextFitPages_.PrepareForGC();
    singleObjectPages_.PrepareForGC();
    for (int blockSize = 0; blockSize <= FixedBlockPage::MAX_BLOCK_SIZE; ++blockSize) {
        fixedBlockPages_[blockSize].PrepareForGC();
    }
    fixedBlockExtraObjectPages_.PrepareForGC();
    singleExtraObjectPages_.PrepareForGC();
    immixBlocks_.PrepareForGC();
}

void Heap::PrepareForYoungGC() noexcept {
    CustomAllocDebug("Heap::PrepareForYoungGC()");
    nextFitPages_.PrepareForYoungGC();
    singleObjectPages_.PrepareForYoungGC();
    for (int blockSize = 0; blockSize <= FixedBlockPage::MAX_BLOCK_SIZE; ++blockSize) {
        fixedBlockPages_[blockSize].PrepareForYoungGC();
    }
    fixedBlockExtraObjectPages_.PrepareForYoungGC();
    singleExtraObjectPages_.PrepareForYoungGC();
    immixBlocks_.PrepareForYoungGC();
}


// Kore: debugging (`KORE_GC_PAUSES`): the addresses of the last `ForEachHeapSlot` that needed the page ranges.
size_t gLastSlowHeapSlots = 0;
size_t gLastHeapRanges = 0;

void Heap::ForEachHeapSlot(
        const std::vector<ObjHeader**>& allSlots, const std::function<void(ObjHeader**, ObjHeader*)>& process) noexcept {
    if (allSlots.empty()) return;
    gLastSlowHeapSlots = 0;
    gLastHeapRanges = 0;

    // Kore: almost every object is in an Immix block, whose object starts give the owner of an address directly. Only
    // the other addresses need the (sorted) ranges of all the heap's pages.
    std::vector<std::pair<const uint8_t*, const uint8_t*>> chunks;
    immixBlocks_.TraverseChunks([&](uint8_t* begin, uint8_t* end) { chunks.emplace_back(begin, end); });
    std::sort(chunks.begin(), chunks.end());
    std::vector<ObjHeader**> sortedSlots;
    auto chunk = chunks.begin();
    for (ObjHeader** slot : allSlots) {
        auto* address = reinterpret_cast<const uint8_t*>(slot);
        while (chunk != chunks.end() && chunk->second <= address) ++chunk;
        if (chunk == chunks.end() || address < chunk->first) {
            sortedSlots.push_back(slot);
            continue;
        }
        uint8_t* block = ImmixBlock::Of(address)->StartAtOrBefore(address);
        if (block == nullptr) continue;
        auto* object = reinterpret_cast<CustomHeapObject*>(block)->object();
        if (object->typeInfoOrMetaRelaxed() == nullptr) continue;
        if (address >= block + ObjectSweepTraits::elementSize(block).inBytes()) continue;
        process(slot, object);
    }
    gc::pausePhase("slots-immix");
    gLastSlowHeapSlots = sortedSlots.size();
    if (sortedSlots.empty()) return;

    // Kore: the other pages of objects, from their registry (see `PageRegistry`), instead of walking every page.
    PageRegistry::WithSortedRanges([&](const std::vector<PageRegistry::Range>& ranges) {
        gLastHeapRanges = ranges.size();
        auto range = ranges.begin();
        for (ObjHeader** slot : sortedSlots) {
            auto* address = reinterpret_cast<const uint8_t*>(slot);
            while (range != ranges.end() && range->end <= address) ++range;
            if (range == ranges.end()) break;
            if (address < range->begin) continue; // Not in the object heap.

            uint8_t* block = nullptr;
            switch (range->kind) {
                case PageRegistry::Kind::kFixedBlock:
                    block = static_cast<FixedBlockPage*>(range->page)->BlockContaining(address);
                    break;
                case PageRegistry::Kind::kNextFit:
                    block = static_cast<NextFitPage*>(range->page)->BlockContaining(address);
                    break;
                case PageRegistry::Kind::kSingleObject:
                    block = static_cast<SingleObjectPage*>(range->page)->Data();
                    break;
            }
            if (block == nullptr) continue;
            auto* object = reinterpret_cast<CustomHeapObject*>(block)->object();
            // Free fixed blocks are zeroed (apart from free-list links stored in the GC header word), and a new single
            // object page may not have its object yet.
            if (object->typeInfoOrMetaRelaxed() == nullptr) continue;
            process(slot, object);
        }
    });
    gc::pausePhase("slots-pages");
}

FinalizerQueue Heap::Sweep(gc::GCHandle gcHandle) noexcept {
    return Sweep(gcHandle, [](const std::function<void()>& task) { task(); });
}

FinalizerQueue Heap::Sweep(gc::GCHandle gcHandle, const std::function<void(const std::function<void()>&)>& runInParallel) noexcept {
    FinalizerQueue finalizerQueue;
    CustomAllocDebug("Heap: before sweep FinalizerQueue size == %zu", finalizerQueue.size());
    CustomAllocDebug("Heap::Sweep()");
    {
        // Pages are taken from the stores atomically, so several threads can sweep the same stores.
        std::mutex finalizerQueueMutex;
        runInParallel([&] {
            FinalizerQueue localQueue;
            {
                auto sweepHandle = gcHandle.sweep();
                for (int blockSize = 0; blockSize <= FixedBlockPage::MAX_BLOCK_SIZE; ++blockSize) {
                    fixedBlockPages_[blockSize].Sweep(sweepHandle, localQueue);
                }
                nextFitPages_.Sweep(sweepHandle, localQueue);
                immixBlocks_.Sweep<ObjectSweepTraits>(sweepHandle, localQueue, allocatedSizeTracker_);
            }
            std::unique_lock guard(finalizerQueueMutex);
            finalizerQueue.mergeFrom(std::move(localQueue));
        });
        // The single-object store is not safe to sweep from several threads.
        auto sweepHandle = gcHandle.sweep();
        singleObjectPages_.Sweep(sweepHandle, finalizerQueue);
    }
    CustomAllocDebug("Heap: before extra sweep FinalizerQueue size == %zu", finalizerQueue.size());
    {
        auto sweepHandle = gcHandle.sweepExtraObjects();
        fixedBlockExtraObjectPages_.Sweep(sweepHandle, finalizerQueue);
        singleExtraObjectPages_.Sweep(sweepHandle, finalizerQueue);
    }
    // wait for concurrent assistants to finish sweeping the last popped page
    while (concurrentSweepersCount_.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }
    CustomAllocDebug("Heap::Sweep done");
    return finalizerQueue;
}

NextFitPage* Heap::GetNextFitPage(uint32_t cellCount, FinalizerQueue& finalizerQueue) noexcept {
    CustomAllocDebug("Heap::GetNextFitPage()");
    return nextFitPages_.GetPage(cellCount, finalizerQueue, concurrentSweepersCount_);
}

FixedBlockPage* Heap::GetFixedBlockPage(uint32_t cellCount, FinalizerQueue& finalizerQueue) noexcept {
    CustomAllocDebug("Heap::GetFixedBlockPage()");
    return fixedBlockPages_[cellCount].GetPage(cellCount, finalizerQueue, concurrentSweepersCount_);
}

SingleObjectPage* Heap::GetSingleObjectPage(uint64_t cellCount) noexcept {
    CustomAllocInfo("CustomAllocator::GetSingleObjectPage(%" PRIu64 ")", cellCount);
    return singleObjectPages_.NewPage(cellCount);
}

FixedBlockPage* Heap::GetFixedBlockExtraObjectPage(FinalizerQueue& finalizerQueue) noexcept {
    CustomAllocInfo("CustomAllocator::GetFixedBlockExtraObjectPage()");
    auto cellCount = ExtraObjectCell::size().inCells();
    return fixedBlockExtraObjectPages_.GetPage(cellCount, finalizerQueue, concurrentSweepersCount_);
}

SingleObjectPage* Heap::GetSingleExtraObjectPage() noexcept {
    CustomAllocInfo("CustomAllocator::GetSingleExtraObjectPage()");
    return singleExtraObjectPages_.NewPage(ExtraObjectCell::size().inCells());
}

void Heap::AddToFinalizerQueue(FinalizerQueue queue) noexcept {
    std::unique_lock guard(pendingFinalizerQueueMutex_);
    pendingFinalizerQueue_.mergeFrom(std::move(queue));
}

FinalizerQueue Heap::ExtractFinalizerQueue() noexcept {
    std::unique_lock guard(pendingFinalizerQueueMutex_);
    return std::move(pendingFinalizerQueue_);
}

std::vector<ObjHeader*> Heap::GetAllocatedObjects() noexcept {
    std::vector<ObjHeader*> allocated;
    for (int blockSize = 0; blockSize <= FixedBlockPage::MAX_BLOCK_SIZE; ++blockSize) {
        for (auto* page : fixedBlockPages_[blockSize].GetPages()) {
            for (auto* block : page->GetAllocatedBlocks()) {
                allocated.push_back(reinterpret_cast<CustomHeapObject*>(block)->object());
            }
        }
    }
    for (auto* page : nextFitPages_.GetPages()) {
        for (auto* block : page->GetAllocatedBlocks()) {
            allocated.push_back(reinterpret_cast<CustomHeapObject*>(block)->object());
        }
    }
    for (auto* page : singleObjectPages_.GetPages()) {
        for (auto* block : page->GetAllocatedBlocks()) {
            allocated.push_back(reinterpret_cast<CustomHeapObject*>(block)->object());
        }
    }
    immixBlocks_.TraverseBlocks([&](ImmixBlock* block) {
        for (auto* object : block->GetAllocatedBlocks()) {
            allocated.push_back(reinterpret_cast<CustomHeapObject*>(object)->object());
        }
    });
    std::vector<ObjHeader*> unfinalized;
    for (auto* block: allocated) {
        if (!block->has_meta_object() || !mm::ExtraObjectData::Get(block)->getFlag(mm::ExtraObjectData::FLAGS_FINALIZED)) {
            unfinalized.push_back(block);
        }
    }
    return unfinalized;
}

void Heap::ClearForTests() noexcept {
    for (int blockSize = 0; blockSize <= FixedBlockPage::MAX_BLOCK_SIZE; ++blockSize) {
        fixedBlockPages_[blockSize].Clear();
    }
    nextFitPages_.Clear();
    singleObjectPages_.Clear();
    fixedBlockExtraObjectPages_.Clear();
    singleExtraObjectPages_.Clear();
    immixBlocks_.Clear();
}

} // namespace kotlin::alloc
