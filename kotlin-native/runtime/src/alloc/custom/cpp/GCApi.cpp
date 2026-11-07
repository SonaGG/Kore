/*
 * Copyright 2022 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#include "GCApi.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifndef KONAN_WINDOWS
#include <sys/mman.h>
#endif

// Remove after bootstrap brings KONAN_APPLE
#ifndef KONAN_APPLE
#define KONAN_APPLE (KONAN_OSX | KONAN_IOS | KONAN_TVOS | KONAN_WATCHOS)
#endif

#if KONAN_APPLE
#include <mach/vm_statistics.h>
#endif

#include "CompilerConstants.hpp"
#include "CustomAllocator.hpp"
#include "CustomLogging.hpp"
#include "ExtraObjectData.hpp"
#include "FinalizerHooks.hpp"
#include "GC.hpp"
#include "GCStatistics.hpp"
#include "HeapReservation.hpp"
#include "KAssert.h"
#include "Memory.h"

namespace {
std::atomic<size_t> allocatedBytesCounter;
}

bool kotlin::alloc::ObjectSweepTraits::trySweepElement(uint8_t* object, FinalizerQueue& finalizerQueue, ObjectSweepTraits::GCSweepScope& gcHandle) noexcept {
    auto* heapObject = reinterpret_cast<CustomHeapObject*>(object);
    // Kore: evacuation (docs/kore/gc-redesign.md, phase 5): the header of an object copied elsewhere is the copy's address
    // with tag 2. The original is garbage.
    if (compiler::immixEvacuation() &&
        (__atomic_load_n(reinterpret_cast<uintptr_t*>(heapObject->object()), __ATOMIC_RELAXED) & OBJECT_TAG_MASK) == 2) {
        gcHandle.addSweptObject();
        return true;
    }
    auto size = CustomAllocator::GetAllocatedHeapSize(heapObject->object());
    if (gc::tryResetMark(heapObject->heapHeader())) {
        CustomAllocDebug("SweepObject(%p): still alive", heapObject);
        gcHandle.addKeptObject(size);
        return false;
    }
    auto* extraObject = mm::ExtraObjectData::Get(heapObject->object());
    if (extraObject) {
        if (!extraObject->getFlag(mm::ExtraObjectData::FLAGS_IN_FINALIZER_QUEUE)) {
            CustomAllocDebug("SweepObject(%p): needs to be finalized, extraObject at %p", heapObject, extraObject);
            extraObject->setFlag(mm::ExtraObjectData::FLAGS_IN_FINALIZER_QUEUE);
            extraObject->ClearRegularWeakReferenceImpl();
            CustomAllocDebug("SweepObject: fromExtraObject(%p) = %p", extraObject, ExtraObjectCell::fromExtraObject(extraObject));
            auto* cell = ExtraObjectCell::fromExtraObject(extraObject);
            if (compiler::objcDisposeOnMain() && extraObject->getFlag(mm::ExtraObjectData::FLAGS_RELEASE_ON_MAIN_QUEUE)) {
                finalizerQueue.mainThread.Push(cell);
            } else {
                finalizerQueue.regular.Push(cell);
            }
            if (HasFinalizersDataInObject(heapObject->object())) {
                // The object must survive until the finalizers for it are finished.
                gcHandle.addMarkedObject();
                gcHandle.addKeptObject(size);
                return false;
            }
            // The object has a finalizer, but all the data for it resides in `ExtraObjectData`. So, detach the object from it, and free it.
            extraObject->UnlinkFromBaseObject();
            CustomAllocDebug("SweepObject(%p): can be reclaimed", heapObject);
            gcHandle.addSweptObject();
            return true;
        }
        if (!extraObject->getFlag(mm::ExtraObjectData::FLAGS_FINALIZED)) {
            CustomAllocDebug("SweepObject(%p): already waiting to be finalized", heapObject);
            gcHandle.addMarkedObject();
            gcHandle.addKeptObject(size);
            return false;
        }
        extraObject->UnlinkFromBaseObject();
        extraObject->setFlag(mm::ExtraObjectData::FLAGS_SWEEPABLE);
    }
    CustomAllocDebug("SweepObject(%p): can be reclaimed", heapObject);
    gcHandle.addSweptObject();
    return true;
}

kotlin::alloc::AllocationSize kotlin::alloc::ObjectSweepTraits::elementSize(uint8_t* data) {
    auto* object = reinterpret_cast<kotlin::alloc::CustomHeapObject*>(data)->object();
    return AllocationSize::bytesAtLeast(CustomAllocator::GetAllocatedHeapSize(object));
}

bool kotlin::alloc::ExtraDataSweepTraits::trySweepElement(uint8_t* element, FinalizerQueue&, ExtraDataSweepTraits::GCSweepScope& gcHandle) noexcept {
    auto* extraObject = (reinterpret_cast<ExtraObjectCell*>(element))->Data();
    if (extraObject->getFlag(mm::ExtraObjectData::FLAGS_SWEEPABLE)) {
        gcHandle.addSweptObject();
        CustomAllocDebug("SweepExtraObject(%p): can be reclaimed", extraObject);
        return true;
    }
    gcHandle.addKeptObject(sizeof(mm::ExtraObjectData));
    CustomAllocDebug("SweepExtraObject(%p): is still needed", extraObject);
    return false;
}

kotlin::alloc::AllocationSize kotlin::alloc::ExtraDataSweepTraits::elementSize(uint8_t*) {
    return ExtraObjectCell::size();
}

namespace {

// Kore: pages are at least 32-byte aligned (for `ExtraObjectCell`s, see `Kotlin_objectHeaderFlagsMask`). `calloc`
// guarantees less: it gets 40 bytes more, and the word before the aligned memory keeps what `free` needs.
constexpr size_t kSystemAllocAlignment = 32;

void* callocAligned(size_t size) noexcept {
    auto* raw = static_cast<uint8_t*>(calloc(size + kSystemAllocAlignment + sizeof(void*), 1));
    if (raw == nullptr) return nullptr;
    auto aligned = (reinterpret_cast<uintptr_t>(raw) + sizeof(void*) + kSystemAllocAlignment - 1) & ~(kSystemAllocAlignment - 1);
    reinterpret_cast<void**>(aligned)[-1] = raw;
    return reinterpret_cast<void*>(aligned);
}

void freeAligned(void* memory) noexcept {
    free(static_cast<void**>(memory)[-1]);
}

} // namespace

static void* systemAlloc(uint64_t size) noexcept {
    if (size > std::numeric_limits<size_t>::max()) {
        konan::consoleErrorf("Out of memory trying to allocate %" PRIu64 " bytes. Aborting.\n", size);
        std::abort();
    }
    void* memory;
    bool error;
    if (kotlin::compiler::disableMmap()) {
        memory = callocAligned(size);
        error = memory == nullptr;
    } else {
#if KONAN_WINDOWS
        RuntimeFail("mmap is not available on mingw");
#elif KONAN_LINUX
        memory = mmap(nullptr, size, PROT_WRITE | PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE | MAP_POPULATE, -1, 0);
        error = memory == MAP_FAILED;
#elif KONAN_APPLE
        auto tag = compiler::mmapTag();
        memory = mmap(nullptr, size, PROT_WRITE | PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, tag ? VM_MAKE_TAG(tag) : -1, 0);
        error = memory == MAP_FAILED;
#else
        memory = mmap(nullptr, size, PROT_WRITE | PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
        error = memory == MAP_FAILED;
#endif
    }
    if (error) {
        konan::consoleErrorf("Out of memory trying to allocate %" PRIu64 " bytes: %s. Aborting.\n", size, strerror(errno));
        std::abort();
    }
    return memory;
}

static void systemFree(void* ptr, size_t size) noexcept {
    if (kotlin::compiler::disableMmap()) {
        freeAligned(ptr);
    } else {
#if KONAN_WINDOWS
        RuntimeFail("mmap is not available on mingw");
#else
        auto result = munmap(ptr, size);
        RuntimeAssert(result == 0, "Failed to munmap: %s", strerror(errno));
#endif
    }
}

namespace {

// Kore: freed large blocks (pages of large objects, which are allocated and freed per object, e.g. the arrays of
// growing lists) are kept for reuse, up to a bound, instead of going back to the system: reusing them saves the
// system calls and the page faults (and zeroing by the system) of fresh memory. Large blocks have size classes (four
// per power of two), so that a block fits allocations of similar sizes.
constexpr size_t kCachedBlockMinBytes = 64 * 1024;
// The cache bound; `KORE_LARGE_BLOCK_CACHE_MB` overrides it.
size_t cachedBytesMax() noexcept {
    static const size_t value = [] {
        if (const char* text = std::getenv("KORE_LARGE_BLOCK_CACHE_MB")) return static_cast<size_t>(std::strtoull(text, nullptr, 10)) * 1024 * 1024;
        // 16 MiB: enough for the arrays a growing collection goes through, without keeping much memory resident.
        return static_cast<size_t>(16) * 1024 * 1024;
    }();
    return value;
}

size_t blockSize(size_t size) noexcept {
    if (size < kCachedBlockMinBytes) return size;
    size_t power = size_t(1) << (63 - __builtin_clzll(size));
    size_t step = power / 4;
    return (size + step - 1) / step * step;
}

struct BlockCache {
    std::mutex mutex;
    std::unordered_map<size_t, std::vector<void*>> blocks; // By block size.
    // The block size of each large block in use: callers may free a block with a different size than they allocated
    // it with (e.g. `SingleObjectPage` recomputes it from its object), which must not put it in the wrong size class.
    std::unordered_map<void*, size_t> blockSizes;
    size_t bytes = 0;
};

BlockCache& blockCache() noexcept {
    [[clang::no_destroy]] static BlockCache cache;
    return cache;
}

} // namespace

thread_local kotlin::alloc::UnzeroedRange kotlin::alloc::tlsSingleObjectPageUnzeroed;

void* kotlin::alloc::SafeAlloc(uint64_t size) noexcept {
    return SafeAlloc(size, 0, 0);
}

void* kotlin::alloc::SafeAlloc(uint64_t size, size_t unzeroedFrom, size_t unzeroedLength) noexcept {
    void* memory = nullptr;
    if (auto* reservation = HeapReservation::instance()) {
        memory = reservation->allocate(static_cast<size_t>(size), HeapReservation::kPageSize, unzeroedFrom, unzeroedLength);
        if (memory != nullptr) {
            allocatedBytesCounter.fetch_add(static_cast<size_t>(size), std::memory_order_relaxed);
            return memory;
        }
        if (compiler::compressedRefs()) {
            // Kore: compressed references can only refer to objects in the reservation.
            konan::consoleErrorf("Out of memory trying to allocate %" PRIu64 " bytes: the Kotlin heap reservation is full%s. Aborting.\n", size,
                                 compiler::compressedRefsZeroBased()
                                         ? " (zero-based compressed references keep the heap below 4 GiB; "
                                           "-Xbinary=compressedRefsZeroBased=false lifts the limit)"
                                         : "");
            std::abort();
        }
    }
    if (size >= kCachedBlockMinBytes && size <= std::numeric_limits<size_t>::max()) {
        size_t block = blockSize(static_cast<size_t>(size));
        auto& cache = blockCache();
        {
            std::unique_lock guard(cache.mutex);
            auto it = cache.blocks.find(block);
            if (it != cache.blocks.end() && !it->second.empty()) {
                memory = it->second.back();
                it->second.pop_back();
                cache.bytes -= block;
            }
        }
        if (memory != nullptr) {
            // Reused memory is zeroed here (fresh system memory already is), except for the bytes the caller overwrites.
            auto bytes = static_cast<size_t>(size);
            if (unzeroedLength == 0 || unzeroedFrom >= bytes) {
                std::memset(memory, 0, bytes);
            } else {
                size_t unzeroedEnd = std::min(bytes, unzeroedFrom + unzeroedLength);
                std::memset(memory, 0, unzeroedFrom);
                std::memset(static_cast<uint8_t*>(memory) + unzeroedEnd, 0, bytes - unzeroedEnd);
            }
        } else {
            memory = systemAlloc(block);
        }
        std::unique_lock guard(cache.mutex);
        cache.blockSizes[memory] = block;
    } else {
        memory = systemAlloc(size);
    }
    allocatedBytesCounter.fetch_add(static_cast<size_t>(size), std::memory_order_relaxed);
    CustomAllocDebug("SafeAlloc(%zu) = %p", static_cast<size_t>(size), memory);
    return memory;
}

void kotlin::alloc::Free(void* ptr, size_t size) noexcept {
    CustomAllocDebug("Free(%p, %zu)", ptr, size);
    allocatedBytesCounter.fetch_sub(size, std::memory_order_relaxed);
    if (auto* reservation = HeapReservation::instance(); reservation != nullptr && reservation->contains(ptr)) {
        reservation->free(ptr, size);
        return;
    }
    auto& cache = blockCache();
    size_t block;
    {
        std::unique_lock guard(cache.mutex);
        auto it = cache.blockSizes.find(ptr);
        if (it == cache.blockSizes.end()) {
            guard.unlock();
            systemFree(ptr, size);
            return;
        }
        block = it->second;
        cache.blockSizes.erase(it);
        if (cache.bytes + block <= cachedBytesMax()) {
            cache.blocks[block].push_back(ptr);
            cache.bytes += block;
            return;
        }
    }
    systemFree(ptr, block);
}

void* kotlin::alloc::SafeAllocAligned(uint64_t size, size_t alignment) noexcept {
    auto* reservation = HeapReservation::instance();
    if (reservation == nullptr) return nullptr;
    void* memory = reservation->allocate(static_cast<size_t>(size), alignment, 0, 0);
    if (memory != nullptr) allocatedBytesCounter.fetch_add(static_cast<size_t>(size), std::memory_order_relaxed);
    return memory;
}

size_t kotlin::alloc::GetAllocatedBytes() noexcept {
    return allocatedBytesCounter.load(std::memory_order_relaxed);
}
