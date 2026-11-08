/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#include "HeapReservation.hpp"

#include "Allocator.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

// As in GCApi.cpp: the bootstrap compiler does not define KONAN_APPLE yet.
#ifndef KONAN_APPLE
#define KONAN_APPLE (KONAN_OSX | KONAN_IOS | KONAN_TVOS | KONAN_WATCHOS)
#endif

#if KONAN_WINDOWS
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/mman.h>
#if KONAN_APPLE
#include <mach-o/loader.h>
#else
#include <link.h>
#endif
#endif

#include <vector>

#include "CompilerConstants.hpp"
#include "CompressedRefs.hpp"
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

// Kore: compressed references (docs/kore/gc-redesign.md, phase 3). The heap and the image (whose permanent objects
// references can point to) must lie within `kCompressedWindow` above `Kotlin_heapBase`. Finds the largest free range of
// address space next to the image that fits, reserves it and sets the base. Aborts if there is none.
constexpr size_t kCompressedWindow = size_t(32) << 30;
constexpr size_t kGranularity = 64 * 1024;

struct ImageRange {
    uintptr_t start;
    uintptr_t end;
};

ImageRange imageRange() noexcept {
#if KONAN_WINDOWS
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&imageRange), &module);
    auto start = reinterpret_cast<uintptr_t>(module);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(start);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(start + dos->e_lfanew);
    return {start, start + nt->OptionalHeader.SizeOfImage};
#elif KONAN_APPLE
    // The image's Mach-O header, then its segments (all but the unmapped __PAGEZERO), moved by the ASLR slide.
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&imageRange), &info) == 0 || info.dli_fbase == nullptr) return {0, 0};
    auto* header = static_cast<const mach_header_64*>(info.dli_fbase);
    auto* command = reinterpret_cast<const load_command*>(header + 1);
    uintptr_t low = UINTPTR_MAX, high = 0;
    intptr_t slide = 0;
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (command->cmd == LC_SEGMENT_64) {
            auto* segment = reinterpret_cast<const segment_command_64*>(command);
            if (std::strcmp(segment->segname, SEG_PAGEZERO) != 0) {
                if (segment->fileoff == 0 && segment->filesize != 0) {
                    slide = reinterpret_cast<intptr_t>(header) - static_cast<intptr_t>(segment->vmaddr);
                }
                low = std::min(low, static_cast<uintptr_t>(segment->vmaddr));
                high = std::max(high, static_cast<uintptr_t>(segment->vmaddr + segment->vmsize));
            }
        }
        command = reinterpret_cast<const load_command*>(reinterpret_cast<const uint8_t*>(command) + command->cmdsize);
    }
    if (high == 0) return {0, 0};
    return {low + slide, high + slide};
#else
    Dl_info info{};
    dladdr(reinterpret_cast<void*>(&imageRange), &info);
    struct Search {
        uintptr_t address;
        ImageRange range;
    } search{reinterpret_cast<uintptr_t>(&imageRange), {0, 0}};
    dl_iterate_phdr(
            [](dl_phdr_info* phdr, size_t, void* data) -> int {
                auto* search = static_cast<Search*>(data);
                uintptr_t low = UINTPTR_MAX, high = 0;
                for (int i = 0; i < phdr->dlpi_phnum; ++i) {
                    if (phdr->dlpi_phdr[i].p_type != PT_LOAD) continue;
                    auto segmentStart = static_cast<uintptr_t>(phdr->dlpi_addr + phdr->dlpi_phdr[i].p_vaddr);
                    low = std::min(low, segmentStart);
                    high = std::max(high, static_cast<uintptr_t>(segmentStart + phdr->dlpi_phdr[i].p_memsz));
                }
                if (search->address < low || search->address >= high) return 0;
                search->range = {low, high};
                return 1;
            },
            &search);
    return search.range;
#endif
}

uint8_t* reserveAt(uintptr_t address, size_t bytes) noexcept {
#if KONAN_WINDOWS
    return static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(address), bytes, MEM_RESERVE, PAGE_NOACCESS));
#else
    void* memory = mmap(reinterpret_cast<void*>(address), bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (memory == MAP_FAILED) return nullptr;
    if (reinterpret_cast<uintptr_t>(memory) != address) {
        munmap(memory, bytes);
        return nullptr;
    }
    return static_cast<uint8_t*>(memory);
#endif
}

alloc::HeapReservation* reserveCompressed(size_t maxBytes) noexcept {
    ImageRange image = imageRange();
    if (image.end == 0) {
        konan::consoleErrorf("Kore: compressed references: could not find the image. Aborting.\n");
        std::abort();
    }
    uintptr_t imageStart = image.start & ~(kGranularity - 1);
    uintptr_t imageEnd = (image.end + kGranularity - 1) & ~(kGranularity - 1);
    // The window is the 32 GiB-aligned span holding the image: with an aligned base, encoding needs no base (and null
    // encodes to 0 by itself), see `encodeRef`.
    uintptr_t base = imageStart & ~(kCompressedWindow - 1);
    if (imageEnd > base + kCompressedWindow) {
        konan::consoleErrorf("Kore: compressed references: the image crosses a 32 GiB boundary. Aborting.\n");
        std::abort();
    }
    // Free ranges of the window, the heap starting `kCompressedRefsHeapOffset` above the base or more (see the
    // conservative stack scan).
    uintptr_t from = base + kCompressedRefsHeapOffset;
    uintptr_t to = base + kCompressedWindow;
    struct Candidate {
        uintptr_t start;
        uintptr_t end;
    };
    std::vector<Candidate> candidates;
    auto consider = [&](uintptr_t start, uintptr_t end) {
        start = std::max((start + kGranularity - 1) & ~(kGranularity - 1), from);
        end = std::min(end & ~(kGranularity - 1), to);
        // Not overlapping the image.
        if (start < imageEnd && end > imageStart) {
            if (start < imageStart) end = imageStart; else start = imageEnd;
        }
        if (end <= start) return;
        if (end - start > maxBytes) end = start + maxBytes;
        candidates.push_back({start, end});
    };
#if KONAN_WINDOWS
    for (uintptr_t address = from; address < to;) {
        MEMORY_BASIC_INFORMATION info;
        if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == 0) break;
        auto regionStart = reinterpret_cast<uintptr_t>(info.BaseAddress);
        uintptr_t regionEnd = regionStart + info.RegionSize;
        if (info.State == MEM_FREE) consider(std::max(regionStart, from), std::min(regionEnd, to));
        address = regionEnd;
    }
#else
    // Without a portable map of the address space: try ranges of decreasing sizes on both sides of the image.
    for (size_t bytes = kCompressedWindow; bytes >= (size_t(256) << 20); bytes /= 2) {
        if (imageStart > from + bytes) consider(imageStart - bytes, imageStart);
        consider(imageEnd, imageEnd + bytes);
        consider(from, from + bytes);
    }
#endif
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.end - a.start > b.end - b.start; });
    for (auto& candidate : candidates) {
        size_t bytes = candidate.end - candidate.start;
        if (uint8_t* memory = reserveAt(candidate.start, bytes)) {
            Kotlin_heapBase = base;
            return new alloc::HeapReservation(memory, bytes);
        }
    }
    konan::consoleErrorf("Kore: compressed references: no address space for the Kotlin heap near the image. Aborting.\n");
    std::abort();
}

// Kore: zero-based compressed references: the heap and the image lie below 4 GiB, and a reference is the low 32 bits of
// the address. Reserves the largest free range of address space below 4 GiB (the executable is linked low).
alloc::HeapReservation* reserveZeroBased(size_t maxBytes) noexcept {
    constexpr uintptr_t kLimit = uintptr_t(1) << 32;
    // Above the first 64 MiB: small integers on the stack never point into the heap.
    constexpr uintptr_t kLow = uintptr_t(64) << 20;
    // Free ranges this close together are reserved together, the space between them being holes (e.g. the shared user
    // data page at 2 GiB on Windows, which splits the low 4 GiB in two).
    constexpr uintptr_t kMaxHole = uintptr_t(64) << 20;
    ImageRange image = imageRange();
    if (image.end == 0 || image.end > kLimit) {
        konan::consoleErrorf("Kore: zero-based compressed references: the image is not below 4 GiB. Aborting.\n");
        std::abort();
    }
    std::vector<std::pair<uintptr_t, uintptr_t>> regions;
    auto consider = [&](uintptr_t start, uintptr_t end) {
        start = std::max((start + kGranularity - 1) & ~(kGranularity - 1), kLow);
        end = std::min(end & ~(kGranularity - 1), kLimit);
        if (end > start + (uintptr_t(16) << 20)) regions.push_back({start, end});
    };
#if KONAN_WINDOWS
    for (uintptr_t address = kGranularity; address < kLimit;) {
        MEMORY_BASIC_INFORMATION info;
        if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == 0) break;
        auto regionStart = reinterpret_cast<uintptr_t>(info.BaseAddress);
        uintptr_t regionEnd = regionStart + info.RegionSize;
        if (info.State == MEM_FREE) consider(regionStart, regionEnd);
        address = regionEnd;
    }
#else
    consider(uintptr_t(1) << 30, kLimit);
#endif
    // The cluster of nearby free ranges with the most space.
    size_t bestFirst = 0, bestLast = 0, bestBytes = 0;
    for (size_t first = 0; first < regions.size();) {
        size_t last = first;
        size_t bytes = regions[first].second - regions[first].first;
        while (last + 1 < regions.size() && regions[last + 1].first - regions[last].second <= kMaxHole) {
            ++last;
            bytes += regions[last].second - regions[last].first;
        }
        if (bytes > bestBytes) {
            bestFirst = first;
            bestLast = last;
            bestBytes = bytes;
        }
        first = last + 1;
    }
    if (bestBytes != 0) {
        uintptr_t start = regions[bestFirst].first;
        uintptr_t end = regions[bestLast].second;
        if (end - start > maxBytes) end = start + maxBytes;
        std::vector<std::pair<uintptr_t, uintptr_t>> holes;
        bool reserved = true;
        for (size_t i = bestFirst; i <= bestLast && regions[i].first < end; ++i) {
            uintptr_t regionEnd = std::min(regions[i].second, end);
            if (reserveAt(regions[i].first, regionEnd - regions[i].first) == nullptr) {
                reserved = false;
                break;
            }
            if (i + 1 <= bestLast && regions[i + 1].first < end) holes.push_back({regionEnd, regions[i + 1].first});
        }
        if (reserved) {
            Kotlin_heapBase = 0;
            return new alloc::HeapReservation(reinterpret_cast<uint8_t*>(start), end - start, holes);
        }
    }
    konan::consoleErrorf("Kore: zero-based compressed references: no address space for the Kotlin heap below 4 GiB. Aborting.\n");
    std::abort();
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
        if (compiler::compressedRefsZeroBased()) return reserveZeroBased(bytes == 0 ? kCompressedWindow : bytes);
        if (compiler::compressedRefs()) return reserveCompressed(bytes == 0 ? kCompressedWindow : bytes);
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

alloc::HeapReservation::HeapReservation(uint8_t* base, size_t bytes, const std::vector<std::pair<uintptr_t, uintptr_t>>& holes) noexcept :
    HeapReservation(base, bytes) {
    // Everything is handed out from free runs: the pages around the holes.
    frontier_ = pageCount_;
    committed_.resize((static_cast<size_t>(pageCount_) + 63) / 64);
    Page start = 0;
    for (auto& hole : holes) {
        Page holeStart = pageOf(reinterpret_cast<void*>(hole.first));
        Page holeEnd = static_cast<Page>((hole.second - reinterpret_cast<uintptr_t>(base) + kPageSize - 1) / kPageSize);
        if (holeStart > start) insertFreeRun(start, holeStart - start);
        start = holeEnd;
    }
    if (pageCount_ > start) insertFreeRun(start, pageCount_ - start);
}

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

void kotlin::alloc::initCompressedRefsBase() noexcept {
    HeapReservation::instance();
}
