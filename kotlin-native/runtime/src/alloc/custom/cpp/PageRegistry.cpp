/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#include "PageRegistry.hpp"

#include <algorithm>

using namespace kotlin::alloc;

std::mutex& PageRegistry::mutex() noexcept {
    [[clang::no_destroy]] static std::mutex instance;
    return instance;
}

std::vector<PageRegistry::Range>& PageRegistry::ranges() noexcept {
    [[clang::no_destroy]] static std::vector<Range> instance;
    return instance;
}

void PageRegistry::Add(const uint8_t* begin, const uint8_t* end, Kind kind, void* page) noexcept {
    std::unique_lock guard(mutex());
    auto& all = ranges();
    auto position = std::upper_bound(all.begin(), all.end(), begin, [](const uint8_t* address, const Range& range) {
        return address < range.begin;
    });
    all.insert(position, Range{begin, end, kind, page});
}

void PageRegistry::Remove(void* page) noexcept {
    std::unique_lock guard(mutex());
    auto& all = ranges();
    // Pages are found by their range's start, which lies inside the page (after its header).
    auto position = std::upper_bound(all.begin(), all.end(), static_cast<const uint8_t*>(page), [](const uint8_t* address, const Range& range) {
        return address < range.begin;
    });
    if (position != all.end() && position->page == page) {
        all.erase(position);
        return;
    }
    // Not found where expected: fall back to a scan (does nothing for pages that were never added).
    auto found = std::find_if(all.begin(), all.end(), [page](const Range& range) { return range.page == page; });
    if (found != all.end()) all.erase(found);
}
