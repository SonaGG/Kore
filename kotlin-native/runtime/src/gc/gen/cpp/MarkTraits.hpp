/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#pragma once

#include "GC.hpp"
#include "IntrusiveList.hpp"
#include "ObjectData.hpp"

namespace kotlin::gc::internal {

using MarkQueue = intrusive_forward_list<GC::ObjectData>;

// Same as the stop-the-world collector's traits. Old objects are already marked, so `tryEnqueue` and `tryMark` fail for
// them: marking stops at the old generation without any extra check.
struct MarkTraits {
    using MarkQueue = MarkQueue;

    static constexpr auto kAllowHeapToStackRefs = true;

    static void clear(MarkQueue& queue) noexcept { queue.clear(); }

    static ObjHeader* tryDequeue(MarkQueue& queue) noexcept {
        if (auto* top = queue.try_pop_front()) {
            return alloc::objectForObjectData(*top);
        }
        return nullptr;
    }

    static bool tryEnqueue(MarkQueue& queue, ObjHeader* object) noexcept {
        return queue.try_push_front(alloc::objectDataForObject(object));
    }

    static bool tryMark(ObjHeader* object) noexcept { return alloc::objectDataForObject(object).tryMark(); }

    static void processInMark(MarkQueue& markQueue, ObjHeader* object) noexcept {
        auto process = object->type_info()->processObjectInMark;
        RuntimeAssert(process != nullptr, "Got null processObjectInMark for object %p", object);
        process(static_cast<void*>(&markQueue), object);
    }
};

} // namespace kotlin::gc::internal
