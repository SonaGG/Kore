/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace kotlin::alloc {

// Kore: memory management v3, phase 1 (docs/kore/gc-redesign.md). One reserved range of address space for the whole
// object heap, committed on demand and handed out in runs of pages; freed runs stay committed up to a bound and are
// decommitted (returned to the system) beyond it. Later phases encode references relative to `base()`.
//
// The size is `KORE_HEAP_RESERVE_GB` (default 32); 0 disables the reservation (the heap then uses the system allocator,
// as before).
class HeapReservation {
public:
    static constexpr size_t kPageSize = 4096;

    // The reservation, or null if it is disabled or could not be reserved. Thread-safe.
    static HeapReservation* instance() noexcept;

    bool contains(const void* address) const noexcept {
        auto* byte = static_cast<const uint8_t*>(address);
        return byte >= base_ && byte < end_;
    }
    uint8_t* base() const noexcept { return base_; }

    // `bytes` (rounded up to pages) at an `alignment` (a power of two, at least a page), zeroed except for the
    // `unzeroedLength` bytes from `unzeroedFrom`. Null when the reservation is exhausted.
    void* allocate(size_t bytes, size_t alignment, size_t unzeroedFrom, size_t unzeroedLength) noexcept;
    void free(void* memory, size_t bytes) noexcept;

    // For memory that stays allocated but unused (free Immix blocks): returns its pages to the system, and makes them
    // accessible again (zeroed). `decommit` makes the memory inaccessible until `commit`.
    void decommit(void* memory, size_t bytes) noexcept;
    void commit(void* memory, size_t bytes) noexcept;

    // Statistics.
    size_t committedBytes() const noexcept;
    size_t retainedFreeBytes() const noexcept;

    HeapReservation(uint8_t* base, size_t bytes) noexcept;
    // Kore: a reservation with holes: ranges of its address space (owned by others) that are never handed out.
    HeapReservation(uint8_t* base, size_t bytes, const std::vector<std::pair<uintptr_t, uintptr_t>>& holes) noexcept;

private:

    using Page = uint32_t;

    uint8_t* address(Page page) const noexcept { return base_ + static_cast<size_t>(page) * kPageSize; }
    Page pageOf(const void* address) const noexcept {
        return static_cast<Page>((static_cast<const uint8_t*>(address) - base_) / kPageSize);
    }

    void insertFreeRun(Page start, Page length) noexcept;
    void eraseFreeRun(Page start, Page length) noexcept;
    // Committed pages in [start, start + length).
    size_t committedPages(Page start, Page length) const noexcept;
    void setCommitted(Page start, Page length, bool committed) noexcept;
    void commitPages(Page start, Page length) noexcept;
    void decommitPages(Page start, Page length) noexcept;
    // Zeroes the committed pages of a run being handed out (fresh pages are zero already), except the unzeroed range.
    void zeroCommitted(Page start, Page length, size_t unzeroedFrom, size_t unzeroedLength) noexcept;

    uint8_t* base_;
    uint8_t* end_;
    Page pageCount_;

    mutable std::mutex mutex_;
    Page frontier_ = 0; // Pages at and above it were never handed out.
    std::map<Page, Page> freeByStart_; // Free runs below the frontier: start -> length.
    std::set<std::pair<Page, Page>> freeBySize_; // (length, start).
    std::vector<uint64_t> committed_; // One bit per page below the frontier.
    size_t committedPageCount_ = 0;
    size_t retainedFreePages_ = 0; // Committed pages in free runs.
};

} // namespace kotlin::alloc
