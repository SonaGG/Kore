/*
 * Copyright 2022 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#ifndef CUSTOM_ALLOC_CPP_PAGESTORE_HPP_
#define CUSTOM_ALLOC_CPP_PAGESTORE_HPP_

#include <atomic>
#include <cstdint>
#include <vector>

#include "AtomicStack.hpp"
#include "GCStatistics.hpp"
#include "FixedBlockPage.hpp"

namespace kotlin::alloc {

template <class T, typename SweepTraits>
class PageStore {
public:
    using GCSweepScope = typename SweepTraits::GCSweepScope;

    void PrepareForGC() noexcept {
        unswept_.TransferAllFrom(std::move(ready_));
        unswept_.TransferAllFrom(std::move(used_));
        unswept_.TransferAllFrom(std::move(parked_));
        T* page;
        while ((page = empty_.Pop())) page->Destroy();
    }

    // Kore: young-generation collection (generational GC with sticky marks). Pages in `ready_` were swept by the
    // previous collection and have not been allocated into since, so they hold only objects that survived it, i.e.
    // old objects. Only pages handed out since the previous collection (`used_`) can hold young objects.
    void PrepareForYoungGC() noexcept {
        generational_.store(true, std::memory_order_relaxed);
        unswept_.TransferAllFrom(std::move(used_));
        T* page;
        while ((page = empty_.Pop())) page->Destroy();
    }

    void Sweep(GCSweepScope& sweepHandle, FinalizerQueue& finalizerQueue) noexcept {
        while (SweepSingle(sweepHandle, unswept_.Pop(), unswept_, ready_, finalizerQueue)) {
        }
    }

    T* GetPage(uint32_t cellCount, FinalizerQueue& finalizerQueue, std::atomic<std::size_t>& concurrentSweepersCount_) noexcept {
        T* page;
        while ((page = ready_.Pop())) {
            // Kore: with a generational GC, every page handed out here is swept by the next young collection, old
            // objects included. Don't allocate young objects into mostly-old pages; leave them for the next full
            // collection.
            if (generational_.load(std::memory_order_relaxed) && page->LiveFraction() > kMaxLiveFractionForYoungAllocation) {
                parked_.Push(page);
                continue;
            }
            used_.Push(page);
            return page;
        }
        {
            auto handle = gc::GCHandle::currentEpoch();
            ScopeGuard counterGuard(
                    [&]() { ++concurrentSweepersCount_; },
                    [&]() { --concurrentSweepersCount_; }
            );

            // Kore: with a generational GC, sweeping right after a young collection covers all the pages allocated
            // since the previous one. A mutator that helped with it would stall for milliseconds, so it takes an empty
            // or new page instead and leaves sweeping to the GC thread, which keeps refilling `ready_`.
            if (!generational_.load(std::memory_order_relaxed) && (page = unswept_.Pop())) {
                // If there're unswept_ pages, the GC is in progress.
                auto sweepHandle = SweepTraits::currentGCSweepScope(*handle);
                if ((page = SweepSingle(sweepHandle, page, unswept_, used_, finalizerQueue))) {
                    return page;
                }
            }
        }
        if ((page = empty_.Pop())) {
            used_.Push(page);
            return page;
        }
        return NewPage(cellCount);
    }

    T* NewPage(uint64_t cellCount) noexcept {
        T* page = T::Create(cellCount);
        used_.Push(page);
        return page;
    }

    bool isEmpty() noexcept {
        return empty_.isEmpty() &&
            ready_.isEmpty() &&
            used_.isEmpty() &&
            unswept_.isEmpty() &&
            parked_.isEmpty();
    }

    ~PageStore() noexcept {
        Clear();
    }

private:
    friend class Heap;

    T* SweepSingle(GCSweepScope& sweepHandle, T* page, AtomicStack<T>& from, AtomicStack<T>& to, FinalizerQueue& finalizerQueue) noexcept {
        if (!page) {
            return nullptr;
        }
        do {
            if (page->template Sweep<SweepTraits>(sweepHandle, finalizerQueue)) {
                to.Push(page);
                return page;
            }
            empty_.Push(page);
        } while ((page = from.Pop()));
        return nullptr;
    }

    // Testing method
    std::vector<T*> GetPages() noexcept {
        std::vector<T*> pages;
        for (T* page : ready_.GetElements()) pages.push_back(page);
        for (T* page : used_.GetElements()) pages.push_back(page);
        for (T* page : unswept_.GetElements()) pages.push_back(page);
        for (T* page : parked_.GetElements()) pages.push_back(page);
        return pages;
    }

    void Clear() noexcept {
        while (T* page = empty_.Pop()) page->Destroy();
        while (T* page = ready_.Pop()) page->Destroy();
        while (T* page = used_.Pop()) page->Destroy();
        while (T* page = unswept_.Pop()) page->Destroy();
        while (T* page = parked_.Pop()) page->Destroy();
        generational_.store(false, std::memory_order_relaxed);
    }

    template <typename F>
    void TraversePages(F process) noexcept(noexcept(process(std::declval<T*>()))) {
        empty_.TraverseElements(process);
        ready_.TraverseElements(process);
        used_.TraverseElements(process);
        unswept_.TraverseElements(process);
        parked_.TraverseElements(process);
    }

    // Kore: see GetPage.
    static constexpr float kMaxLiveFractionForYoungAllocation = 0.5f;

    AtomicStack<T> empty_;
    AtomicStack<T> ready_;
    AtomicStack<T> used_;
    AtomicStack<T> unswept_;
    AtomicStack<T> parked_; // Kore: mostly-old pages withheld from young allocation until the next full collection.
    std::atomic<bool> generational_ = false; // Kore: set once a young-generation collection has run.
};

template <typename SweepTraits>
class PageStore<SingleObjectPage, SweepTraits> {
public:
    using GCSweepScope = typename SweepTraits::GCSweepScope;

    void PrepareForGC() noexcept {
        unswept_.TransferAllFrom(std::move(newlyAllocated_));
        // used_ is not accessed by mutators and will be processed directly in Sweep
        sweepUsed_ = true;
    }

    // Kore: young-generation collection. Objects in `used_` survived a previous collection (old objects under a
    // sticky-mark generational GC), so only pages allocated since then are swept.
    void PrepareForYoungGC() noexcept {
        unswept_.TransferAllFrom(std::move(newlyAllocated_));
        sweepUsed_ = false;
    }

    void Sweep(GCSweepScope& sweepHandle, FinalizerQueue& finalizerQueue) noexcept {
        AtomicStack<SingleObjectPage> survived_{};
        while (auto* page = unswept_.PopNonAtomic()) {
            if (page->SweepAndDestroy<SweepTraits>(sweepHandle, finalizerQueue)) {
                survived_.PushNonAtomic(page);
            }
        }
        if (sweepUsed_) {
            while (auto* page = used_.PopNonAtomic()) {
                if (page->SweepAndDestroy<SweepTraits>(sweepHandle, finalizerQueue)) {
                    survived_.PushNonAtomic(page);
                }
            }
        }
        used_.TransferAllFrom(std::move(survived_));
        sweepUsed_ = true;
    }

    SingleObjectPage* NewPage(uint64_t cellCount) noexcept {
        auto* page = SingleObjectPage::Create(cellCount);
        newlyAllocated_.Push(page);
        return page;
    }

    bool isEmpty() noexcept {
        return used_.isEmpty() && unswept_.isEmpty();
    }

    ~PageStore() noexcept {
        Clear();
    }

private:
    friend class Heap;

    // Testing method
    std::vector<SingleObjectPage*> GetPages() noexcept {
        std::vector<SingleObjectPage*> pages;
        for (auto* page : newlyAllocated_.GetElements()) pages.push_back(page);
        for (auto* page : used_.GetElements()) pages.push_back(page);
        for (auto* page : unswept_.GetElements()) pages.push_back(page);
        return pages;
    }

    void Clear() noexcept {
        while (auto* page = newlyAllocated_.Pop()) page->Destroy<SweepTraits>();
        while (auto* page = used_.Pop()) page->Destroy<SweepTraits>();
        while (auto* page = unswept_.Pop()) page->Destroy<SweepTraits>();
    }

    template <typename F>
    void TraversePages(F process) noexcept(noexcept(process(std::declval<SingleObjectPage*>()))) {
        newlyAllocated_.TraverseElements(process);
        used_.TraverseElements(process);
        unswept_.TraverseElements(process);
    }

    AtomicStack<SingleObjectPage> newlyAllocated_;
    AtomicStack<SingleObjectPage> used_;
    AtomicStack<SingleObjectPage> unswept_;
    bool sweepUsed_ = true; // Kore: false during a young-generation collection.
};

} // namespace kotlin::alloc

#endif
