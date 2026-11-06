/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#include "HeapReservation.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#if KONAN_WINDOWS
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "Porting.h"

using namespace kotlin;

namespace {

size_t environmentSize(const char* name, size_t defaultValue, size_t unit) noexcept {
    if (const char* text = std::getenv(name)) return static_cast<size_t>(std::strtoull(text, nullptr, 10)) * unit;
    return defaultValue * unit;
}

// Committed free memory kept for reuse (saving the system calls, page faults and zeroing of fresh memory); beyond it,
// freed memory goes back to the system.
size_t retainedFreeBytesMax() noexcept {
    static const size_t value = environmentSize("KORE_HEAP_RETAIN_MB", 16, 1024 * 1024);
    return value;
}

uint8_t* reserve(size_t bytes) noexcept {
#if KONAN_WINDOWS
    return static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS));
#else
    void* memory = mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return memory == MAP_FAILED ? nullptr : static_cast<uint8_t*>(memory);
#endif
}

[[noreturn]] void outOfMemory(size_t bytes) noexcept {
    konan::consoleErrorf("Out of memory trying to commit %zu bytes of the Kotlin heap. Aborting.\n", bytes);
    std::abort();
}

} // namespace

alloc::HeapReservation* alloc::HeapReservation::instance() noexcept {
    static HeapReservation* reservation = []() -> HeapReservation* {
        if constexpr (sizeof(void*) < 8) return nullptr;
        size_t bytes = environmentSize("KORE_HEAP_RESERVE_GB", 32, size_t(1) << 30);
        // Page numbers are 32-bit.
        bytes = std::min(bytes, (size_t(1) << 32) * kPageSize - kPageSize);
        if (bytes == 0) return nullptr;
        // Smaller reservations if the address space is limited.
        while (bytes >= (size_t(1) << 30)) {
            if (uint8_t* base = reserve(bytes)) return new HeapReservation(base, bytes);
            bytes /= 2;
        }
        konan::consoleErrorf("Kore: could not reserve address space for the Kotlin heap; using the system allocator.\n");
        return nullptr;
    }();
    return reservation;
}

alloc::HeapReservation::HeapReservation(uint8_t* base, size_t bytes) noexcept :
    base_(base), end_(base + bytes), pageCount_(static_cast<Page>(bytes / kPageSize)) {}

void* alloc::HeapReservation::allocate(size_t bytes, size_t alignment, size_t unzeroedFrom, size_t unzeroedLength) noexcept {
    Page pages = static_cast<Page>((bytes + kPageSize - 1) / kPageSize);
    Page alignPages = static_cast<Page>(std::max<size_t>(1, alignment / kPageSize));
    auto alignUp = [alignPages](Page page) { return (page + alignPages - 1) / alignPages * alignPages; };
    std::unique_lock guard(mutex_);
    // Best fit among the free runs (a few tries for aligned requests), else fresh pages at the frontier.
    Page start = 0;
    bool found = false;
    int tries = 0;
    for (auto it = freeBySize_.lower_bound({pages, 0}); it != freeBySize_.end() && tries < 64; ++it, ++tries) {
        auto [runLength, runStart] = *it;
        Page aligned = alignUp(runStart);
        if (aligned + pages > runStart + runLength) continue;
        eraseFreeRun(runStart, runLength);
        if (aligned > runStart) insertFreeRun(runStart, aligned - runStart);
        if (aligned + pages < runStart + runLength) insertFreeRun(aligned + pages, runStart + runLength - aligned - pages);
        retainedFreePages_ -= committedPages(aligned, pages);
        start = aligned;
        found = true;
        break;
    }
    if (!found) {
        Page aligned = alignUp(frontier_);
        if (static_cast<size_t>(aligned) + pages > pageCount_) return nullptr;
        if (aligned > frontier_) insertFreeRun(frontier_, aligned - frontier_);
        frontier_ = aligned + pages;
        size_t words = (static_cast<size_t>(frontier_) + 63) / 64;
        if (committed_.size() < words) committed_.resize(std::max(words, committed_.size() * 2));
        start = aligned;
    }
    zeroCommitted(start, pages, unzeroedFrom, unzeroedLength);
    if (committedPages(start, pages) != pages) commitPages(start, pages);
    return address(start);
}

void alloc::HeapReservation::free(void* memory, size_t bytes) noexcept {
    Page start = pageOf(memory);
    Page length = static_cast<Page>((bytes + kPageSize - 1) / kPageSize);
    std::unique_lock guard(mutex_);
    retainedFreePages_ += committedPages(start, length);
    auto next = freeByStart_.find(start + length);
    if (next != freeByStart_.end()) {
        Page nextLength = next->second;
        eraseFreeRun(start + length, nextLength);
        length += nextLength;
    }
    auto previous = freeByStart_.lower_bound(start);
    if (previous != freeByStart_.begin()) {
        --previous;
        if (previous->first + previous->second == start) {
            Page previousStart = previous->first;
            Page previousLength = previous->second;
            eraseFreeRun(previousStart, previousLength);
            start = previousStart;
            length += previousLength;
        }
    }
    insertFreeRun(start, length);
    if (retainedFreePages_ * kPageSize > retainedFreeBytesMax()) {
        retainedFreePages_ -= committedPages(start, length);
        decommitPages(start, length);
    }
}

void alloc::HeapReservation::decommit(void* memory, size_t bytes) noexcept {
    std::unique_lock guard(mutex_);
    decommitPages(pageOf(memory), static_cast<Page>((bytes + kPageSize - 1) / kPageSize));
}

void alloc::HeapReservation::commit(void* memory, size_t bytes) noexcept {
    std::unique_lock guard(mutex_);
    commitPages(pageOf(memory), static_cast<Page>((bytes + kPageSize - 1) / kPageSize));
}

size_t alloc::HeapReservation::committedBytes() const noexcept {
    std::unique_lock guard(mutex_);
    return committedPageCount_ * kPageSize;
}

size_t alloc::HeapReservation::retainedFreeBytes() const noexcept {
    std::unique_lock guard(mutex_);
    return retainedFreePages_ * kPageSize;
}

void alloc::HeapReservation::insertFreeRun(Page start, Page length) noexcept {
    freeByStart_.emplace(start, length);
    freeBySize_.emplace(length, start);
}

void alloc::HeapReservation::eraseFreeRun(Page start, Page length) noexcept {
    freeByStart_.erase(start);
    freeBySize_.erase({length, start});
}

size_t alloc::HeapReservation::committedPages(Page start, Page length) const noexcept {
    size_t count = 0;
    size_t page = start;
    size_t end = static_cast<size_t>(start) + length;
    while (page < end) {
        size_t word = page / 64;
        size_t bit = page % 64;
        size_t take = std::min<size_t>(64 - bit, end - page);
        uint64_t mask = take == 64 ? ~uint64_t(0) : ((uint64_t(1) << take) - 1) << bit;
        count += __builtin_popcountll(committed_[word] & mask);
        page += take;
    }
    return count;
}

void alloc::HeapReservation::setCommitted(Page start, Page length, bool committed) noexcept {
    size_t page = start;
    size_t end = static_cast<size_t>(start) + length;
    while (page < end) {
        size_t word = page / 64;
        size_t bit = page % 64;
        size_t take = std::min<size_t>(64 - bit, end - page);
        uint64_t mask = take == 64 ? ~uint64_t(0) : ((uint64_t(1) << take) - 1) << bit;
        uint64_t before = committed_[word];
        committed_[word] = committed ? (before | mask) : (before & ~mask);
        size_t changed = __builtin_popcountll((before ^ committed_[word]) & mask);
        committedPageCount_ = committed ? committedPageCount_ + changed : committedPageCount_ - changed;
        page += take;
    }
}

void alloc::HeapReservation::commitPages(Page start, Page length) noexcept {
    size_t bytes = static_cast<size_t>(length) * kPageSize;
#if KONAN_WINDOWS
    if (VirtualAlloc(address(start), bytes, MEM_COMMIT, PAGE_READWRITE) == nullptr) outOfMemory(bytes);
#else
    if (mprotect(address(start), bytes, PROT_READ | PROT_WRITE) != 0) outOfMemory(bytes);
#endif
    setCommitted(start, length, true);
}

void alloc::HeapReservation::decommitPages(Page start, Page length) noexcept {
    size_t bytes = static_cast<size_t>(length) * kPageSize;
#if KONAN_WINDOWS
    VirtualFree(address(start), bytes, MEM_DECOMMIT);
#else
    // Replacing the pages drops their contents (zeroed when committed again) on every system.
    mmap(address(start), bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
#endif
    setCommitted(start, length, false);
}

void alloc::HeapReservation::zeroCommitted(Page start, Page length, size_t unzeroedFrom, size_t unzeroedLength) noexcept {
    uint8_t* begin = address(start);
    size_t unzeroedBegin = unzeroedLength == 0 ? 0 : unzeroedFrom;
    size_t unzeroedEnd = unzeroedLength == 0 ? 0 : unzeroedFrom + unzeroedLength;
    size_t end = static_cast<size_t>(start) + length;
    size_t page = start;
    while (page < end) {
        // A run of committed pages.
        while (page < end && (committed_[page / 64] & (uint64_t(1) << (page % 64))) == 0) ++page;
        size_t runStart = page;
        while (page < end && (committed_[page / 64] & (uint64_t(1) << (page % 64))) != 0) ++page;
        if (runStart == page) continue;
        size_t from = (runStart - start) * kPageSize;
        size_t to = (page - start) * kPageSize;
        // Minus the unzeroed range.
        if (unzeroedEnd <= from || unzeroedBegin >= to) {
            std::memset(begin + from, 0, to - from);
        } else {
            if (unzeroedBegin > from) std::memset(begin + from, 0, unzeroedBegin - from);
            if (unzeroedEnd < to) std::memset(begin + unzeroedEnd, 0, to - unzeroedEnd);
        }
    }
}
