/*
 * Copyright 2010-2023 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "GC.hpp"
#include "GCStatistics.hpp"
#include "Utils.hpp"
#include "Memory.h"

namespace kotlin::alloc {

// TODO: Move allocator-specific data and API here.
class Allocator : private Pinned {
public:
    class Impl;

    class ThreadData : private Pinned {
    public:
        class Impl;

        explicit ThreadData(Allocator& allocator) noexcept;
        ~ThreadData();

        Impl& impl() noexcept { return *impl_; }

        ObjHeader* allocateObject(const TypeInfo* typeInfo) noexcept;
        ArrayHeader* allocateArray(const TypeInfo* typeInfo, uint32_t elements) noexcept;
        mm::ExtraObjectData& allocateExtraObjectData(ObjHeader* object, const TypeInfo* typeInfo) noexcept;
        void destroyUnattachedExtraObjectData(mm::ExtraObjectData& extraObject) noexcept;

        void prepareForGC() noexcept;

        // TODO: Move into AllocatorTestSupport.hpp
        void clearForTests() noexcept;

    private:
        std::unique_ptr<Impl> impl_;
    };

    Allocator() noexcept;
    ~Allocator();

    Impl& impl() noexcept { return *impl_; }

    void prepareForGC() noexcept;

    // Kore: generational GC support. Like prepareForGC, but only the part of the heap allocated into since the
    // previous collection will be swept. Requires a GC whose marks are sticky (never reset by the sweeper).
    void prepareForYoungGC() noexcept;

    // Kore: generational GC support. Sorts `slots`, then calls `process(slot, object)` for every slot that lies
    // inside an allocated heap object. Other addresses (stack objects, globals, thread-locals) are skipped.
    // Must be called with the world stopped.
    void forEachHeapSlot(std::vector<ObjHeader**>& slots, const std::function<void(ObjHeader**, ObjHeader*)>& process) noexcept;

    // Kore: bytes of heap objects as reported to the GC scheduler (after a sweep: the bytes kept in the whole heap,
    // including pages a young-generation collection did not sweep).
    size_t objectBytes() noexcept;

    // TODO: Move into AllocatorTestSupport.hpp
    void clearForTests() noexcept;

    void TraverseAllocatedObjects(std::function<void(ObjHeader*)> fn) noexcept;

    void TraverseAllocatedExtraObjects(std::function<void(mm::ExtraObjectData*)> fn) noexcept;

    void startFinalizerThreadIfNeeded() noexcept;
    void stopFinalizerThreadIfRunning() noexcept;
    bool finalizersThreadIsRunning() noexcept;

    void configureMainThreadFinalizerProcessor(std::function<void(alloc::RunLoopFinalizerProcessorConfig&)> f) noexcept;
    bool mainThreadFinalizerProcessorAvailable() noexcept;

    void sweep(gc::GCHandle gcHandle) noexcept;
    // Kore: like sweep, but the object pages may be swept by several threads: `runInParallel(task)` must run `task` on
    // the calling thread and on helper threads, and return when all of them have finished.
    void sweep(gc::GCHandle gcHandle, const std::function<void(const std::function<void()>&)>& runInParallel) noexcept;
    void scheduleFinalization(gc::GCHandle gcHandle) noexcept;

    // Kore: moving young generation. Allocates `bytes` in the regular heap, for a nursery object a young collection
    // moves there (GC thread, world stopped); `finishEvacuation` ends that.
    uint8_t* allocateForEvacuation(size_t bytes) noexcept;
    void finishEvacuation() noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

void initObjectPool() noexcept;
// Instruct the allocator to free unused resources.
void compactObjectPoolInCurrentThread() noexcept;

gc::GC::ObjectData& objectDataForObject(ObjHeader* object) noexcept;
ObjHeader* objectForObjectData(gc::GC::ObjectData& objectData) noexcept;

// This does not take into account how much storage did the underlying allocator reserved.
size_t allocatedHeapSize(ObjHeader* object) noexcept;

size_t allocatedBytes() noexcept;

void destroyExtraObjectData(mm::ExtraObjectData& extraObject) noexcept;

// Kore: the moving young generation ("nursery"). Small objects are bump-allocated into thread-local blocks of a
// contiguous nursery. A young collection copies the surviving ones into the regular heap, except pinned ones (referenced
// from a stack or a root, or whose identity was taken), which stay in place and make their block part of the old
// generation ("retained"). Collection functions must be called with the world stopped.
namespace nursery {
bool enabled() noexcept;
// Whether `object` is in a nursery block that the next young collection evacuates.
bool movable(const ObjHeader* object) noexcept;
// Keeps `object` in place (until it is promoted, after which old objects never move).
void pin(const ObjHeader* object) noexcept;
bool pinned(const ObjHeader* object) noexcept;

void setAllocationEnabled(bool enabled) noexcept;
// Ends the thread-local allocation blocks of all threads.
void retireAll() noexcept;
// The movable object whose memory contains `address`, if any (conservative stack scanning).
ObjHeader* movableObjectContaining(uintptr_t address) noexcept;
void forEachMovableObject(const std::function<void(ObjHeader*)>& process) noexcept;
// `object` stays in place: its block is retained.
void retain(const ObjHeader* object) noexcept;
// Frees the evacuated blocks, retains the others, clears the pins.
void finishYoungCollection() noexcept;

bool inRetainedBlock(const void* address) noexcept;
// Debugging: whether `address` is in a free (released) nursery block.
bool inFreeBlock(const void* address) noexcept;
size_t activeBlocks() noexcept;
ObjHeader* retainedObjectContaining(uintptr_t address) noexcept;
void forEachRetainedObject(const std::function<void(ObjHeader*)>& process) noexcept;
// Frees the retained blocks without objects for which `alive` holds.
void releaseEmptyRetainedBlocks(const std::function<bool(ObjHeader*)>& alive) noexcept;
} // namespace nursery
}
