/*
 * Copyright 2010-2021 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "ExtraObjectData.hpp"
#include "GCScheduler.hpp"
#include "concurrent/Mutex.hpp"
#include "ReferenceOps.hpp"
#include "RunLoopFinalizerProcessor.hpp"
#include "TypeLayout.hpp"
#include "Utils.hpp"

namespace kotlin {

namespace alloc {
class Allocator;
}

namespace mm {
class ThreadData;
}

namespace gc {

class GC : private Pinned {
public:
    class Impl;

    class ThreadData : private Pinned {
    public:
        class Impl;

        ThreadData(GC& gc, mm::ThreadData& threadData) noexcept;
        ~ThreadData();

        Impl& impl() noexcept { return *impl_; }

        void OnSuspendForGC() noexcept;

        void safePoint() noexcept;

        void onThreadRegistration() noexcept;

        void onAllocation(ObjHeader* object) noexcept;

    private:
        std::unique_ptr<Impl> impl_;
    };

    // Header to be placed before each heap object. GC will use this to keep its data if needed.
    // This is used via `type_layout::descriptor_t`, which is specialized below.
    // If GC doesn't need any data, it can make `size()` return 0 and `alignment()`
    // return 1.
    // Note: GC does not deinitialize `ObjectData`, so the implementations must ensure that
    //       the destructor is a trivial one.
    class ObjectData;

    GC(alloc::Allocator& allocator, gcScheduler::GCScheduler& gcScheduler) noexcept;
    ~GC();

    Impl& impl() noexcept { return *impl_; }

    void ClearForTests() noexcept;

    static void processObjectInMark(void* state, ObjHeader* object) noexcept;
    static void processArrayInMark(void* state, ArrayHeader* array) noexcept;

    // TODO: These should exist only in the scheduler.
    int64_t Schedule() noexcept;
    // Kore: like Schedule(), but a generational GC may collect only the young generation in this epoch.
    // Non-generational GCs perform a regular collection.
    int64_t ScheduleYoung() noexcept;
    void WaitFinished(int64_t epoch) noexcept;
    void WaitFinalizers(int64_t epoch) noexcept;

    auto gcLock() noexcept {
        return std::unique_lock{gcLock_};
    }

    void onEpochFinalized(int64_t epoch) noexcept;

private:
    std::unique_ptr<Impl> impl_;
    ThreadStateAware<std::mutex> gcLock_{};
};

void beforeHeapRefUpdate(mm::DirectRefAccessor ref, ObjHeader* value, bool loadAtomic) noexcept;
// Kore: like beforeHeapRefUpdate (non-atomic store), for a static field (a global or thread-local root).
void beforeStaticRefUpdate(mm::DirectRefAccessor ref, ObjHeader* value) noexcept;
// Kore: like beforeHeapRefUpdate (non-atomic store), for a slot known to belong to `holder` (a heap, stack or permanent
// object). Used by `UpdateHeapRefInObject`, which compiled code calls for field stores and the runtime for array stores.
void beforeHeapRefUpdateInObject(ObjHeader* holder, mm::DirectRefAccessor ref, ObjHeader* value) noexcept;
// Kore: generational GC, out of line part of `beforeHeapRefUpdateInObject`.
void rememberOldHolderIfYoungValue(ObjHeader* holder, ObjHeader* value) noexcept;
// Kore: must be called after `value` was stored into a reference slot that belongs to `holder` but was written without
// going through `beforeHeapRefUpdate` (e.g. linking a weak reference implementation from ExtraObjectData).
// Generational GCs record old->young edges here; other GCs ignore it.
void afterReferenceStoreInObject(ObjHeader* holder, ObjHeader* value) noexcept;
// Kore: copies `count` references from `source` to the slots at `destination` (which may overlap, like `memmove`), all
// belonging to `holder`, as `count` stores with `beforeHeapRefUpdateInObject` would.
void copyHeapRefsInObject(ObjHeader* holder, ObjHeader** destination, ObjHeader* const* source, size_t count) noexcept;
OBJ_GETTER(weakRefReadBarrier, std_support::atomic_ref<ObjHeader*> weakReferee) noexcept;

bool isMarked(ObjHeader* object) noexcept;

// This will drop the mark bit if it was set and return `true`.
// If the mark bit was unset, this will return `false`.
bool tryResetMark(GC::ObjectData& objectData) noexcept;

namespace barriers {

class ExternalRCRefReleaseGuard : MoveOnly {
    class Impl;
public:
    static bool isNoop();

    ExternalRCRefReleaseGuard(mm::DirectRefAccessor ref) noexcept;
    ExternalRCRefReleaseGuard(ExternalRCRefReleaseGuard&& other) noexcept;
    ~ExternalRCRefReleaseGuard() noexcept;

    ExternalRCRefReleaseGuard& operator=(ExternalRCRefReleaseGuard&& other) noexcept;

private:
    FlatPImpl<Impl, 32> impl_;
};

} // namespace barriers

} // namespace gc

template <>
struct type_layout::descriptor<gc::GC::ObjectData> {
    struct type {
        using value_type = gc::GC::ObjectData;

        static uint64_t size() noexcept;
        static size_t alignment() noexcept;

        static value_type* construct(uint8_t* ptr) noexcept;
    };
};

} // namespace kotlin
