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
#include "IntrusiveList.hpp"
#include "KAssert.h"

namespace kotlin::gc {

// One word placed before every heap object:
//   bit 0  `kOld`          sticky: the object survived a collection. Set by the sweeper, cleared only when freed.
//   bit 1  `kRemembered`   the (old) object is logged in a remembered set since the last young collection.
//   bits 2+ mark state     0: unmarked in the current collection; `kMarkedNoQueue`: marked; otherwise a mark queue
//                          link (8-byte aligned pointer).
// Every sweep resets the mark state of the objects it visits (and makes the survivors old), so a full collection can
// start marking without clearing marks first. All updates are atomic read-modify-writes: mutators set `kRemembered`
// while markers and sweepers update the other bits.
class GC::ObjectData {
public:
    static constexpr uintptr_t kOld = 1;
    static constexpr uintptr_t kRemembered = 2;
    static constexpr uintptr_t kFlags = kOld | kRemembered;
    static constexpr uintptr_t kMarkedNoQueue = 4;

    bool tryMark() noexcept { return trySetNext(reinterpret_cast<ObjectData*>(kMarkedNoQueue)); }

    // Allocation during a concurrent full mark: new objects are marked ("allocated black"). Uncontended.
    void markUncontended() noexcept {
        RuntimeAssert(!marked(), "Must not be marked previously");
        word_.fetch_or(kMarkedNoQueue, std::memory_order_relaxed);
    }

    bool marked() const noexcept { return (word_.load(std::memory_order_relaxed) & ~kFlags) != 0; }
    bool old() const noexcept { return (word_.load(std::memory_order_relaxed) & kOld) != 0; }
    // Old, or survived the collection in progress (marked, not swept yet). Either way it will be old after the sweep.
    bool oldOrSurviving() const noexcept { return (word_.load(std::memory_order_relaxed) & ~kRemembered) != 0; }

    // Sweep. Returns whether the object is alive. Survivors become old; the mark state is reset.
    bool sweep(bool fullCollection) noexcept {
        auto word = word_.load(std::memory_order_relaxed);
        while (true) {
            bool isMarked = (word & ~kFlags) != 0;
            if (!isMarked) {
                // A young collection does not mark old objects; they are alive unless a full collection says otherwise.
                return !fullCollection && (word & kOld) != 0;
            }
            if (word_.compare_exchange_weak(word, (word & kRemembered) | kOld, std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    // Mutators: an old (or surviving) object received a young object. Returns `true` if the caller must log it.
    bool tryRemember() noexcept {
        auto word = word_.load(std::memory_order_relaxed);
        if ((word & ~kRemembered) == 0 || (word & kRemembered) != 0) return false;
        return (word_.fetch_or(kRemembered, std::memory_order_relaxed) & kRemembered) == 0;
    }

    bool remembered() const noexcept { return (word_.load(std::memory_order_relaxed) & kRemembered) != 0; }

    void forget() noexcept { word_.fetch_and(~kRemembered, std::memory_order_relaxed); }

private:
    friend struct DefaultIntrusiveForwardListTraits<ObjectData>;

    // Only the flag bits are stripped: a node that is marked but not queued must report a non-null `next` (the
    // sentinel itself), like in the `cms` collector. The mark queue code relies on that.
    ObjectData* next() const noexcept { return reinterpret_cast<ObjectData*>(word_.load(std::memory_order_relaxed) & ~kFlags); }
    void setNext(ObjectData* next) noexcept {
        RuntimeAssert(next, "next cannot be nullptr");
        auto word = word_.load(std::memory_order_relaxed);
        while (!word_.compare_exchange_weak(word, reinterpret_cast<uintptr_t>(next) | (word & kFlags), std::memory_order_relaxed)) {
        }
    }
    bool trySetNext(ObjectData* next) noexcept {
        RuntimeAssert(next, "next cannot be nullptr");
        auto word = word_.load(std::memory_order_relaxed);
        while (true) {
            if ((word & ~kFlags) != 0) return false;
            if (word_.compare_exchange_weak(word, reinterpret_cast<uintptr_t>(next) | (word & kFlags), std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    std::atomic<uintptr_t> word_ = 0;
};
static_assert(std::is_trivially_destructible_v<GC::ObjectData>);

namespace internal {
// Kind of the collection in progress (or the last one). Set by the GC thread before marking; stable until the sweep
// finishes. A young collection neither marks old objects nor frees them.
extern std::atomic<bool> youngCollection;
inline bool isYoungCollection() noexcept { return youngCollection.load(std::memory_order_relaxed); }
} // namespace internal
static_assert(sizeof(GC::ObjectData) == sizeof(void*));
static_assert(kObjectAlignment >= 8, "mark queue links (object addresses) must leave the low 3 bits free");

} // namespace kotlin::gc
