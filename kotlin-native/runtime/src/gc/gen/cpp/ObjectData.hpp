/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

#include "Alignment.hpp"
#include "Allocator.hpp"
#include "GC.hpp"
#include "KAssert.h"
#include "Memory.h"

namespace kotlin::gc {

// Kore: the GC bits of an object:
//   `kMarked`      marked in the current collection.
//   `kOld`         sticky: the object survived a collection. Set by the sweeper, cleared only when freed.
//   `kRemembered`  the (old) object is logged in a remembered set since the last young collection.
// With the custom allocator they are bits 2-4 of the object's header word, next to its type info or meta object pointer
// (see `Kotlin_objectHeaderFlagsMask`), so that they need no memory of their own. Otherwise (meta objects of the other
// allocators are not aligned enough) they are bits 2-4 of the word the allocator places before the object
// (`GC::ObjectData`).
//
// The mark stacks hold object pointers (`MarkStack.hpp`), so the mark state is one bit: whoever sets it pushes the
// object. Every sweep resets the mark bits of the objects it visits (and makes the survivors old), so a full collection
// can start marking without clearing marks first.
//
// All updates are atomic read-modify-writes that keep the rest of the word: mutators set `kRemembered` and install meta
// objects (`ExtraObjectData::Install`), while markers and sweepers update the other bits. The word is accessed through a
// `may_alias` type: elsewhere it is read as a `TypeInfo*`.
class ObjectBits {
public:
    static constexpr uintptr_t kMarked = 4;
    static constexpr uintptr_t kOld = 8;
    static constexpr uintptr_t kRemembered = 16;
    static constexpr uintptr_t kAll = kMarked | kOld | kRemembered;

    // Whether the bits are in the header word.
    ALWAYS_INLINE static bool inHeader() noexcept { return (Kotlin_objectHeaderFlagsMask & kAll) == kAll; }

    ALWAYS_INLINE explicit ObjectBits(ObjHeader* object) noexcept :
        word_(inHeader() ? reinterpret_cast<Word*>(object) : reinterpret_cast<Word*>(&alloc::objectDataForObject(object))) {}

    ALWAYS_INLINE uintptr_t load() const noexcept { return __atomic_load_n(word_, __ATOMIC_RELAXED); }

    // Sets the mark bit. Returns whether it was not set before.
    ALWAYS_INLINE bool tryMark() noexcept {
        if ((load() & kMarked) != 0) return false;
        return (__atomic_fetch_or(word_, kMarked, __ATOMIC_RELAXED) & kMarked) == 0;
    }

    // Allocation during a concurrent full mark: new objects are marked ("allocated black").
    void markUncontended() noexcept {
        RuntimeAssert(!marked(), "Must not be marked previously");
        __atomic_fetch_or(word_, kMarked, __ATOMIC_RELAXED);
    }

    bool marked() const noexcept { return (load() & kMarked) != 0; }
    bool old() const noexcept { return (load() & kOld) != 0; }
    // Old, or survived the collection in progress (marked, not swept yet). Either way it will be old after the sweep.
    bool oldOrSurviving() const noexcept { return (load() & (kOld | kMarked)) != 0; }
    bool remembered() const noexcept { return (load() & kRemembered) != 0; }

    // Sweep. Returns whether the object is alive. Survivors become old; the mark bit is reset.
    bool sweep(bool fullCollection) noexcept {
        uintptr_t word = load();
        while (true) {
            if ((word & kMarked) == 0) {
                // A young collection does not mark old objects; they are alive unless a full collection says otherwise.
                return !fullCollection && (word & kOld) != 0;
            }
            if (__atomic_compare_exchange_n(word_, &word, (word & ~kMarked) | kOld, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                return true;
            }
        }
    }

    // Mutators: an old (or surviving) object received a young object. Returns `true` if the caller must log it.
    bool tryRemember() noexcept {
        uintptr_t word = load();
        if ((word & (kOld | kMarked)) == 0 || (word & kRemembered) != 0) return false;
        return (__atomic_fetch_or(word_, kRemembered, __ATOMIC_RELAXED) & kRemembered) == 0;
    }

    void forget() noexcept { __atomic_fetch_and(word_, ~kRemembered, __ATOMIC_RELAXED); }

    // Replaces all GC bits (e.g. `kOld` for an object promoted in place, 0 for a dead one).
    void reset(uintptr_t bits) noexcept {
        RuntimeAssert((bits & ~kAll) == 0, "Not GC bits: %p", reinterpret_cast<void*>(bits));
        uintptr_t word = load();
        while (!__atomic_compare_exchange_n(word_, &word, (word & ~kAll) | bits, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        }
    }

    uintptr_t bits() const noexcept { return load() & kAll; }

private:
    using Word = uintptr_t __attribute__((may_alias));

    Word* word_;
};

ALWAYS_INLINE inline ObjectBits objectBits(ObjHeader* object) noexcept {
    return ObjectBits(object);
}

// The word the allocator places before every heap object (as long as there is one, see `gcWord`). It holds the GC bits
// when they cannot be in the header (`ObjectBits`). Otherwise it is only used by the moving young generation: during
// the evacuation of a young collection, it holds the new address of a moved object (with bit 0 set).
class GC::ObjectData {
public:
    void forwardTo(ObjHeader* to) noexcept { word_.store(reinterpret_cast<uintptr_t>(to) | kForwarded, std::memory_order_relaxed); }
    bool forwarded() const noexcept { return (word_.load(std::memory_order_relaxed) & kForwarded) != 0; }
    ObjHeader* forwardee() const noexcept {
        return reinterpret_cast<ObjHeader*>(word_.load(std::memory_order_relaxed) & ~uintptr_t(7));
    }
    void clearForwarding() noexcept { word_.store(0, std::memory_order_relaxed); }
    uintptr_t word() const noexcept { return word_.load(std::memory_order_relaxed); }

private:
    static constexpr uintptr_t kForwarded = 1;

    std::atomic<uintptr_t> word_ = 0;
};
static_assert(std::is_trivially_destructible_v<GC::ObjectData>);

namespace internal {
// Kind of the collection in progress (or the last one). Set by the GC thread before marking; stable until the sweep
// finishes. A young collection neither marks old objects nor frees them.
extern std::atomic<bool> youngCollection;
inline bool isYoungCollection() noexcept { return youngCollection.load(std::memory_order_relaxed); }
// Kore: the number of slices of big arrays waiting to be marked (see `GC::processArrayInMark`).
extern std::atomic<size_t> markArraySlicesPending;
} // namespace internal
static_assert(sizeof(GC::ObjectData) == sizeof(void*));
static_assert(kObjectAlignment >= 8, "forwarding addresses must leave the low 3 bits free");
static_assert(kTypeInfoAlignment >= 32, "type infos must leave header bits 2-4 free");

} // namespace kotlin::gc
