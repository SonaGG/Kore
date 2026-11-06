/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "CompilerConstants.hpp"
#include "std_support/Atomic.hpp"

struct ObjHeader;

// Kore: compressed references (docs/kore/gc-redesign.md, phase 3). With `compiler::compressedRefs()`, the reference
// fields of heap objects (also of objects on the stack and of permanent objects) and the elements of `Array`s are 32
// bits: `(address - Kotlin_heapBase) >> 3`, with 0 for null. Everything they can refer to (the heap reservation and the
// permanent objects of the image) lies within 32 GiB above `Kotlin_heapBase`. Locals, globals, thread locals and
// everything outside of Kotlin objects keep full pointers.
extern "C" uintptr_t Kotlin_heapBase;

namespace kotlin {

// The base is 32 GiB-aligned (see `HeapReservation`): `(address - base) >> 3` is `address >> 3` modulo 2^32, and 0 for null.
ALWAYS_INLINE inline uint32_t encodeRef(const ObjHeader* object) noexcept {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(object) >> 3);
}

ALWAYS_INLINE inline ObjHeader* decodeRef(uint32_t ref) noexcept {
    return ref == 0 ? nullptr : reinterpret_cast<ObjHeader*>(Kotlin_heapBase + (static_cast<uintptr_t>(ref) << 3));
}

// The span of addresses references can point to, above `Kotlin_heapBase`. The heap starts `kCompressedRefsHeapOffset` into
// it (see the conservative stack scan).
constexpr uintptr_t kCompressedRefsWindow = uintptr_t(32) << 30;
constexpr uintptr_t kCompressedRefsHeapOffset = uintptr_t(4) << 30;

// The bytes of a reference in a heap object.
ALWAYS_INLINE inline size_t heapRefSize() noexcept {
    return compiler::compressedRefs() ? sizeof(uint32_t) : sizeof(ObjHeader*);
}

// A reference slot of an object: a field, or an element of an `Array`. No GC barriers.
class HeapRefSlot {
public:
    ALWAYS_INLINE explicit HeapRefSlot(void* address) noexcept : address_(address) {}

    ALWAYS_INLINE void* address() const noexcept { return address_; }
    // The slot `count` slots after this one (array elements).
    ALWAYS_INLINE HeapRefSlot operator+(ptrdiff_t count) const noexcept {
        return HeapRefSlot(static_cast<uint8_t*>(address_) + count * static_cast<ptrdiff_t>(heapRefSize()));
    }

    ALWAYS_INLINE ObjHeader* load() const noexcept {
        if (compiler::compressedRefs()) return decodeRef(*static_cast<uint32_t*>(address_));
        return *static_cast<ObjHeader**>(address_);
    }
    ALWAYS_INLINE void store(ObjHeader* value) const noexcept {
        if (compiler::compressedRefs()) {
            *static_cast<uint32_t*>(address_) = encodeRef(value);
        } else {
            *static_cast<ObjHeader**>(address_) = value;
        }
    }
    ALWAYS_INLINE ObjHeader* loadAtomic(std::memory_order order) const noexcept {
        if (compiler::compressedRefs()) return decodeRef(std_support::atomic_ref{*static_cast<uint32_t*>(address_)}.load(order));
        return std_support::atomic_ref{*static_cast<ObjHeader**>(address_)}.load(order);
    }
    ALWAYS_INLINE void storeAtomic(ObjHeader* value, std::memory_order order) const noexcept {
        if (compiler::compressedRefs()) {
            std_support::atomic_ref{*static_cast<uint32_t*>(address_)}.store(encodeRef(value), order);
        } else {
            std_support::atomic_ref{*static_cast<ObjHeader**>(address_)}.store(value, order);
        }
    }
    ALWAYS_INLINE ObjHeader* exchange(ObjHeader* value, std::memory_order order) const noexcept {
        if (compiler::compressedRefs()) {
            return decodeRef(std_support::atomic_ref{*static_cast<uint32_t*>(address_)}.exchange(encodeRef(value), order));
        }
        return std_support::atomic_ref{*static_cast<ObjHeader**>(address_)}.exchange(value, order);
    }
    ALWAYS_INLINE bool compareAndExchange(ObjHeader*& expected, ObjHeader* value, std::memory_order order) const noexcept {
        if (compiler::compressedRefs()) {
            uint32_t expectedRef = encodeRef(expected);
            bool result = std_support::atomic_ref{*static_cast<uint32_t*>(address_)}.compare_exchange_strong(
                    expectedRef, encodeRef(value), order);
            expected = decodeRef(expectedRef);
            return result;
        }
        return std_support::atomic_ref{*static_cast<ObjHeader**>(address_)}.compare_exchange_strong(expected, value, order);
    }

private:
    void* address_;
};

} // namespace kotlin
