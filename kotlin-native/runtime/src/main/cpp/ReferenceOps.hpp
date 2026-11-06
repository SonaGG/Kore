/*
 * Copyright 2010-2023 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#pragma once

#include "CompressedRefs.hpp"
#include "Memory.h"
#include "std_support/Atomic.hpp"

#if __has_feature(thread_sanitizer)
#include <sanitizer/tsan_interface.h>
#endif

// C++ memory model is in some sence stricter than the memmory model of real target CPUs.
// For example all the ptr-sized memory accesses on intel x86 and arm CPUs are atomic.
// Another case is the release-consume memory ordering, which can be achieved without additional memory fences on consume.
//
// However, LLVM often fails to properly optimize atomic operations.
// So we have to allow some imeplementation-defined UB here.
//
// Under this flag all tha operations with references in the kotlin heap
// are implemented in complete complience with C++ memory model.
#define STRICT_ATOMICS_IN_HEAP __has_feature(thread_sanitizer)

namespace kotlin::mm {

// TODO: Make sure these operations work with any kind of thread stopping: safepoints and signals.

// TODO: Consider adding some kind of an `Object` type (that wraps `ObjHeader*`) which
//       will have these operations for a friendlier API.

/**
 * Represents direct low-level operations on Koltin references.
 * No GC barriers are inserted. Should be used with care!
 */
class DirectRefAccessor {
public:
    DirectRefAccessor() = delete;
    DirectRefAccessor& operator=(const DirectRefAccessor&) = delete;

    // A full-pointer slot: a local, a global, a thread local, or memory outside of Kotlin objects.
    explicit DirectRefAccessor(ObjHeader*& fieldRef) noexcept : location_(&fieldRef), narrow_(false) {}
    explicit DirectRefAccessor(ObjHeader** fieldPtr) noexcept : location_(fieldPtr), narrow_(false) {}
    // Kore: a reference slot of an object (a field or an `Array` element): 32-bit with compressed references.
    explicit DirectRefAccessor(HeapRefSlot slot) noexcept : location_(slot.address()), narrow_(compiler::compressedRefs()) {}
    DirectRefAccessor(const DirectRefAccessor& other) = default;

    // The address of the slot (an `ObjHeader**` unless `narrow()`).
    void* address() const noexcept { return location_; }
    bool narrow() const noexcept { return narrow_; }
    ObjHeader** location() const noexcept {
        RuntimeAssert(!narrow_, "A compressed reference slot has no ObjHeader** location");
        return static_cast<ObjHeader**>(location_);
    }

    PERFORMANCE_INLINE operator ObjHeader*() const noexcept { return load(); }
    PERFORMANCE_INLINE ObjHeader* operator=(ObjHeader* desired) noexcept { store(desired); return desired; }

    PERFORMANCE_INLINE ObjHeader* load() const noexcept {
#if STRICT_ATOMICS_IN_HEAP
        auto loaded = loadAtomic(std::memory_order_consume);
#if __has_feature(thread_sanitizer)
        __tsan_acquire(loaded);
#endif
        return loaded;
#else
        if (narrow_) return HeapRefSlot(location_).load();
        return *static_cast<ObjHeader**>(location_);
#endif
    }

    PERFORMANCE_INLINE void store(ObjHeader* desired) noexcept {
#if STRICT_ATOMICS_IN_HEAP
        storeAtomic(desired, std::memory_order_relaxed);
#else
        if (narrow_) {
            HeapRefSlot(location_).store(desired);
        } else {
            *static_cast<ObjHeader**>(location_) = desired;
        }
#endif
    }

    PERFORMANCE_INLINE ObjHeader* loadAtomic(std::memory_order order) const noexcept {
        if (narrow_) return HeapRefSlot(location_).loadAtomic(order);
        return std_support::atomic_ref{*static_cast<ObjHeader**>(location_)}.load(order);
    }
    PERFORMANCE_INLINE void storeAtomic(ObjHeader* desired, std::memory_order order) noexcept {
        if (narrow_) {
            HeapRefSlot(location_).storeAtomic(desired, order);
        } else {
            std_support::atomic_ref{*static_cast<ObjHeader**>(location_)}.store(desired, order);
        }
    }
    PERFORMANCE_INLINE ObjHeader* exchange(ObjHeader* desired, std::memory_order order) noexcept {
        if (narrow_) return HeapRefSlot(location_).exchange(desired, order);
        return std_support::atomic_ref{*static_cast<ObjHeader**>(location_)}.exchange(desired, order);
    }
    PERFORMANCE_INLINE bool compareAndExchange(ObjHeader*& expected, ObjHeader* desired, std::memory_order order) noexcept {
        if (narrow_) return HeapRefSlot(location_).compareAndExchange(expected, desired, order);
        return std_support::atomic_ref{*static_cast<ObjHeader**>(location_)}.compare_exchange_strong(expected, desired, order);
    }

private:
    void* location_;
    bool narrow_;
};

/**
 * Represents Koltin-level operations on Koltin references.
 * With all the necessary GC barriers etc.
 * Prefer using aliases below.
 */
template<bool kOnStack>
class RefAccessor {
public:
    RefAccessor() = delete;
    RefAccessor& operator=(const RefAccessor&) = delete;

    explicit RefAccessor(ObjHeader*& fieldRef) noexcept : direct_(fieldRef) {}
    explicit RefAccessor(ObjHeader** fieldPtr) noexcept : RefAccessor(*fieldPtr) {}
    // Kore: a reference slot of an object (32-bit with compressed references).
    explicit RefAccessor(HeapRefSlot slot) noexcept : direct_(slot) {}
    RefAccessor(const RefAccessor& other) noexcept : direct_(other.direct_) {}

    DirectRefAccessor direct() const noexcept { return direct_; }

    void beforeLoad() noexcept;
    void afterLoad() noexcept;
    void beforeStore(ObjHeader* value) noexcept;
    void afterStore(ObjHeader* value) noexcept;

    PERFORMANCE_INLINE operator ObjHeader*() noexcept { return load(); }

    PERFORMANCE_INLINE ObjHeader* load() noexcept {
        AssertThreadState(ThreadState::kRunnable);
        beforeLoad();
        auto result = direct_.load();
        afterLoad();
        return result;
    }

    PERFORMANCE_INLINE ObjHeader* loadAtomic(std::memory_order order) noexcept {
        AssertThreadState(ThreadState::kRunnable);
        beforeLoad();
        auto result = direct_.loadAtomic(order);
        afterLoad();
        return result;
    }

    PERFORMANCE_INLINE ObjHeader* operator=(ObjHeader* desired) noexcept { store(desired); return desired; }

    PERFORMANCE_INLINE void store(ObjHeader* desired) noexcept {
        AssertThreadState(ThreadState::kRunnable);
        beforeStore(desired);
        direct_.store(desired);
        afterStore(desired);
    }

    PERFORMANCE_INLINE void storeAtomic(ObjHeader* desired, std::memory_order order) noexcept {
        AssertThreadState(ThreadState::kRunnable);
        beforeStore(desired);
        direct_.storeAtomic(desired, order);
        afterStore(desired);
    }

    PERFORMANCE_INLINE ObjHeader* exchange(ObjHeader* desired, std::memory_order order) noexcept {
        AssertThreadState(ThreadState::kRunnable);
        beforeLoad();
        beforeStore(desired);
        auto result = direct_.exchange(desired, order);
        afterStore(desired);
        afterLoad();
        return result;
    }

    PERFORMANCE_INLINE bool compareAndExchange(ObjHeader*& expected, ObjHeader* desired, std::memory_order order) noexcept {
        AssertThreadState(ThreadState::kRunnable);
        beforeLoad();
        beforeStore(desired);
        bool result = direct_.compareAndExchange(expected, desired, order);
        afterStore(desired);
        afterLoad();
        return result;
    }

private:
    DirectRefAccessor direct_;
};

using RefFieldAccessor = RefAccessor<false>;
using GlobalRefAccessor = RefAccessor<false>;
using StackRefAccessor = RefAccessor<true>;

class RefField : private Pinned {
public:
    auto accessor() noexcept {
        return mm::RefFieldAccessor(value_);
    }
    auto direct() noexcept {
        return accessor().direct();
    }
    // FIXME probably most of the uses should instead use accessor
    auto ptr() noexcept {
        return direct().location();
    }

    // TODO consider adding other operations
    ObjHeader* operator=(ObjHeader* value) noexcept {
        accessor() = value;
        return value_;
    }

    bool operator==(const RefField& other) const noexcept {
        return value_ == other.value_;
    }

    bool operator!=(const RefField& other) const noexcept {
        return !operator==(other);
    }

private:
    ObjHeader* value_ = nullptr;
};

OBJ_GETTER(weakRefReadBarrier, std_support::atomic_ref<ObjHeader*> weakReferee) noexcept;

}
