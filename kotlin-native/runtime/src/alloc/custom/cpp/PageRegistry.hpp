/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace kotlin::alloc {

// Kore: the address ranges of the pages that hold objects outside of Immix blocks (fixed-block, next-fit and single
// object pages), sorted, so that the young collection finds the object containing an address (a remembered slot or a
// conservative stack candidate) without walking every page of the heap.
class PageRegistry {
public:
    enum class Kind : uint8_t { kFixedBlock, kNextFit, kSingleObject };

    struct Range {
        const uint8_t* begin;
        const uint8_t* end;
        Kind kind;
        void* page;
    };

    static void Add(const uint8_t* begin, const uint8_t* end, Kind kind, void* page) noexcept;
    // Does nothing for a page that was not added.
    static void Remove(void* page) noexcept;

    // Calls `process` with the sorted ranges, under the registry's lock.
    template <typename F>
    static void WithSortedRanges(F process) noexcept {
        std::unique_lock guard(mutex());
        process(static_cast<const std::vector<Range>&>(ranges()));
    }

private:
    static std::mutex& mutex() noexcept;
    static std::vector<Range>& ranges() noexcept;
};

} // namespace kotlin::alloc
