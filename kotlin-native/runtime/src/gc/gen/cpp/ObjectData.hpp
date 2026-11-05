/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

#include "Allocator.hpp"
#include "GC.hpp"
#include "IntrusiveList.hpp"
#include "KAssert.h"

namespace kotlin::gc {

// One word placed before every heap object.
//   0                     young (unmarked)
//   non-zero              marked = old. Either `kMarked` or a mark queue link (8-byte aligned pointer).
//   bit `kRemembered`     set on an old object that is already logged in an object remembered set.
// Marks are sticky: the sweeper never clears them, so an object that survives a collection becomes old in place.
// Only a full collection clears them (see `reset`).
class GC::ObjectData {
public:
    static constexpr uintptr_t kMarked = 1;
    static constexpr uintptr_t kRemembered = 2;

    bool tryMark() noexcept { return trySetNext(reinterpret_cast<ObjectData*>(kMarked)); }

    bool marked() const noexcept { return word_.load(std::memory_order_relaxed) != 0; }

    // Full collection only: make the object young (and not remembered) again.
    void reset() noexcept { word_.store(0, std::memory_order_relaxed); }

    // Called by mutators when an old object receives a reference to a young one.
    // Returns `true` if the object was not remembered yet, i.e. the caller must log it.
    bool tryRemember() noexcept {
        auto word = word_.load(std::memory_order_relaxed);
        if (word == 0 || (word & kRemembered) != 0) return false;
        return (word_.fetch_or(kRemembered, std::memory_order_relaxed) & kRemembered) == 0;
    }

    bool remembered() const noexcept { return (word_.load(std::memory_order_relaxed) & kRemembered) != 0; }

    // Called by the GC (world stopped) after the logged object was rescanned.
    void forget() noexcept { word_.fetch_and(~kRemembered, std::memory_order_relaxed); }

private:
    friend struct DefaultIntrusiveForwardListTraits<ObjectData>;

    ObjectData* next() const noexcept { return reinterpret_cast<ObjectData*>(word_.load(std::memory_order_relaxed) & ~kRemembered); }
    void setNext(ObjectData* next) noexcept {
        RuntimeAssert(next, "next cannot be nullptr");
        word_.store(reinterpret_cast<uintptr_t>(next), std::memory_order_relaxed);
    }
    bool trySetNext(ObjectData* next) noexcept {
        RuntimeAssert(next, "next cannot be nullptr");
        if (word_.load(std::memory_order_relaxed) != 0) {
            return false;
        }
        word_.store(reinterpret_cast<uintptr_t>(next), std::memory_order_relaxed);
        return true;
    }

    std::atomic<uintptr_t> word_ = 0;
};
static_assert(std::is_trivially_destructible_v<GC::ObjectData>);
static_assert(sizeof(GC::ObjectData) == sizeof(void*));

} // namespace kotlin::gc
