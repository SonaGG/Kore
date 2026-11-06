/*
 * Copyright 2022 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#ifndef CUSTOM_ALLOC_CPP_HEAP_HPP_
#define CUSTOM_ALLOC_CPP_HEAP_HPP_

#include <atomic>
#include <mutex>
#include <cstring>
#include <functional>
#include <vector>

#include "AtomicStack.hpp"
#include "ExtraObjectData.hpp"
#include "GCStatistics.hpp"
#include "Memory.h"
#include "SingleObjectPage.hpp"
#include "NextFitPage.hpp"
#include "PageStore.hpp"
#include "FixedBlockPage.hpp"
#include "GCApi.hpp"
#include "ImmixBlockStore.hpp"

namespace kotlin::alloc {

class Heap {
public:
    // Called once by the GC thread after all mutators have been suspended
    void PrepareForGC() noexcept;

    // Kore: like PrepareForGC, but for a young-generation collection: only pages allocated into since the previous
    // collection will be swept. Requires a sticky-mark generational GC (marks of survivors are never reset).
    void PrepareForYoungGC() noexcept;

    // Kore: for every address in `sortedSlots` (ascending) that lies inside an allocated heap object, calls
    // `process(slot, object)`. Addresses outside the object heap (stack objects, globals, thread-locals) are skipped.
    // Must be called with the world stopped.
    void ForEachHeapSlot(const std::vector<ObjHeader**>& sortedSlots, const std::function<void(ObjHeader**, ObjHeader*)>& process) noexcept;

    // Sweep through all remaining pages, freeing those blocks where CanReclaim
    // returns true. If multiple sweepers are active, each page will only be
    // seen by one sweeper.
    FinalizerQueue Sweep(gc::GCHandle gcHandle) noexcept;
    // Kore: sweeps the multi-object pages with `runInParallel` (see Allocator::sweep), the rest on the calling thread.
    FinalizerQueue Sweep(gc::GCHandle gcHandle, const std::function<void(const std::function<void()>&)>& runInParallel) noexcept;

    FixedBlockPage* GetFixedBlockPage(uint32_t cellCount, FinalizerQueue& finalizerQueue) noexcept;
    NextFitPage* GetNextFitPage(uint32_t cellCount, FinalizerQueue& finalizerQueue) noexcept;
    SingleObjectPage* GetSingleObjectPage(uint64_t cellCount) noexcept;
    FixedBlockPage* GetFixedBlockExtraObjectPage(FinalizerQueue& finalizerQueue) noexcept;
    SingleObjectPage* GetSingleExtraObjectPage() noexcept;

    // Kore: memory v2, see ImmixBlockStore.hpp.
    ImmixBlockStore& immixBlocks() noexcept { return immixBlocks_; }

    void AddToFinalizerQueue(FinalizerQueue queue) noexcept;
    FinalizerQueue ExtractFinalizerQueue() noexcept;

    // Test method
    std::vector<ObjHeader*> GetAllocatedObjects() noexcept;
    void ClearForTests() noexcept;

    auto& allocatedSizeTracker() noexcept { return allocatedSizeTracker_; }

    template <typename T>
    void TraverseAllocatedObjects(T process) noexcept(noexcept(process(std::declval<ObjHeader*>()))) {
        for (int blockSize = 0; blockSize <= FixedBlockPage::MAX_BLOCK_SIZE; ++blockSize) {
            fixedBlockPages_[blockSize].TraversePages([process](auto *page) {
                page->TraverseAllocatedBlocks([process](auto *block) {
                    process(reinterpret_cast<CustomHeapObject*>(block)->object());
                });
            });
        }
        nextFitPages_.TraversePages([process](auto *page) {
            page->TraverseAllocatedBlocks([process](auto *block) {
                process(reinterpret_cast<CustomHeapObject*>(block)->object());
            });
        });
        singleObjectPages_.TraversePages([process](auto *page) {
            page->TraverseAllocatedBlocks([process](auto *block) {
                process(reinterpret_cast<CustomHeapObject*>(block)->object());
            });
        });
        immixBlocks_.TraverseBlocks([process](ImmixBlock* block) {
            block->TraverseAllocatedBlocks([process](uint8_t* object) {
                process(reinterpret_cast<CustomHeapObject*>(object)->object());
            });
        });
    }

    template <typename T>
    void TraverseAllocatedExtraObjects(T process) noexcept(noexcept(process(std::declval<kotlin::mm::ExtraObjectData*>()))) {
        fixedBlockExtraObjectPages_.TraversePages([process](auto *page) {
            page->TraverseAllocatedBlocks([process](uint8_t* block) {
                process(reinterpret_cast<ExtraObjectCell*>(block)->Data());
            });
        });
        singleExtraObjectPages_.TraversePages([process](auto *page) {
            page->TraverseAllocatedBlocks([process](uint8_t* block) {
                process(reinterpret_cast<ExtraObjectCell*>(block)->Data());
            });
        });
    }

private:
    PageStore<FixedBlockPage, ObjectSweepTraits> fixedBlockPages_[FixedBlockPage::MAX_BLOCK_SIZE + 1];
    PageStore<NextFitPage, ObjectSweepTraits> nextFitPages_;
    PageStore<SingleObjectPage, ObjectSweepTraits> singleObjectPages_;
    PageStore<FixedBlockPage, ExtraDataSweepTraits> fixedBlockExtraObjectPages_;
    PageStore<SingleObjectPage, ExtraDataSweepTraits> singleExtraObjectPages_;
    ImmixBlockStore immixBlocks_;

    FinalizerQueue pendingFinalizerQueue_;
    std::mutex pendingFinalizerQueueMutex_;

    std::atomic<std::size_t> concurrentSweepersCount_ = 0;

    AllocatedSizeTracker::Heap allocatedSizeTracker_{};
};

} // namespace kotlin::alloc

#endif
