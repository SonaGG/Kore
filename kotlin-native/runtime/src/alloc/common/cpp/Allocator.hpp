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
}
