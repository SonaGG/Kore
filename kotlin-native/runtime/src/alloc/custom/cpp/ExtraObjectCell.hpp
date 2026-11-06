/*
 * Copyright 2010-2023 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#pragma once

#include "ExtraObjectData.hpp"
#include "std_support/Atomic.hpp"

namespace kotlin::alloc {

struct alignas(32) ExtraObjectCell { // Kore: aligned, see `data_`.
    static constexpr AllocationSize size() {
        return AllocationSize::bytesAtLeast(sizeof(ExtraObjectCell));
    }

    static ExtraObjectCell* fromExtraObject(mm::ExtraObjectData* extraObjectData) {
        return reinterpret_cast<ExtraObjectCell*>(reinterpret_cast<uint8_t*>(extraObjectData) - offsetof(ExtraObjectCell, data_));
    }

    mm::ExtraObjectData* Data() { return reinterpret_cast<mm::ExtraObjectData*>(data_); }

    // Kore: the data first, so that it is as aligned as the cell: object headers that point to it keep bits 2-4 free
    // (see `Kotlin_objectHeaderFlagsMask`). Cells are 32-byte aligned.
    struct alignas(mm::ExtraObjectData) {
        uint8_t data_[sizeof(mm::ExtraObjectData)];
    };

    // This is used to build a finalizers queue.
    ExtraObjectCell* next_ = nullptr;
    auto atomicNext() noexcept {
        return std_support::atomic_ref{next_};
    }
};

// ensure const-evaluatable
static_assert(ExtraObjectCell::size().inCells() > 0);
static_assert(offsetof(ExtraObjectCell, data_) == 0 && sizeof(ExtraObjectCell) % 32 == 0); // Kore

} // namespace kotlin::alloc
