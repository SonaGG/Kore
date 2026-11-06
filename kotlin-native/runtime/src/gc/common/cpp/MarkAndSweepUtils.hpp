/*
 * Copyright 2010-2021 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#ifndef RUNTIME_GC_COMMON_MARK_AND_SWEEP_UTILS_H
#define RUNTIME_GC_COMMON_MARK_AND_SWEEP_UTILS_H

#include "Allocator.hpp"
#include "CompilerConstants.hpp"
#include "ExtraObjectData.hpp"
#include "FinalizerHooks.hpp"
#include "GlobalData.hpp"
#include "GCStatistics.hpp"
#include "Logging.hpp"
#include "Memory.h"
#include "ObjectOps.hpp"
#include "ObjectTraversal.hpp"
#include "RootSet.hpp"
#include "Runtime.h"
#include "ExternalRCRefRegistry.hpp"
#include "ThreadData.hpp"
#include "Types.h"

namespace kotlin {
namespace gc {

namespace internal {

template <typename Traits>
void processFieldInMark(void* state, ObjHeader* object, ObjHeader* field) noexcept {
    auto& markQueue = *static_cast<typename Traits::MarkQueue*>(state);
    if (field->heap()) {
        Traits::tryEnqueue(markQueue, field);
    }
    if constexpr (!Traits::kAllowHeapToStackRefs) {
        if (object->heap()) {
            RuntimeAssert(!field->stack(), "Heap object %p references stack object %p[typeInfo=%p]", object, field, field->type_info());
        }
    }
}

template <typename Traits>
void processObjectInMark(void* state, ObjHeader* object) noexcept {
    traverseClassObjectFields(object, [=] (auto fieldAccessor) noexcept {
        if (ObjHeader* field = fieldAccessor.direct()) {
            processFieldInMark<Traits>(state, object, field);
        }
    });
}

template <typename Traits>
void processArrayInMark(void* state, ArrayHeader* array) noexcept {
    traverseArrayOfObjectsElements(array, [=] (auto elemAccessor) noexcept {
        if (ObjHeader* elem = elemAccessor.direct()) {
            processFieldInMark<Traits>(state, array->obj(), elem);
        }
    });
}

template <typename Traits>
bool collectRoot(typename Traits::MarkQueue& markQueue, ObjHeader* object) noexcept {
    if (isNullOrMarker(object))
        return false;

    if (object->heap()) {
        Traits::tryEnqueue(markQueue, object);
    } else {
        // Each permanent and stack object has own entry in the root set, so it's okay to only process objects in heap.
        Traits::processInMark(markQueue, object);
        RuntimeAssert(!object->has_meta_object(), "Non-heap object %p may not have an extra object data", object);
    }
    return true;
}

// TODO: Consider making it noinline to keep loop in `Mark` small.
template <typename Traits>
void processExtraObjectData(GCHandle::GCMarkScope& markHandle, typename Traits::MarkQueue& markQueue, mm::ExtraObjectData& extraObjectData, ObjHeader* object) noexcept {
    if (auto weakReference = extraObjectData.GetRegularWeakReferenceImpl()) {
        RuntimeAssert(
                weakReference->heap(), "Weak reference must be a heap object. object=%p weak=%p permanent=%d stack=%d", object,
                weakReference, weakReference->permanent(), weakReference->stack());
        // Do not schedule RegularWeakReferenceImpl but process it right away.
        // This will skip markQueue interaction.
        if (Traits::tryMark(weakReference)) {
            markHandle.addObject();
            // RegularWeakReferenceImpl is empty, but keeping this just in case.
            Traits::processInMark(markQueue, weakReference);
        }
    }
}

} // namespace internal

template <typename Traits>
void Mark(GCHandle handle, typename Traits::MarkQueue& markQueue) noexcept {
    auto markHandle = handle.mark();
    Mark<Traits>(markHandle, markQueue);
}

template <typename Traits>
void Mark(GCHandle::GCMarkScope& markHandle, typename Traits::MarkQueue& markQueue) noexcept {
    while (ObjHeader* top = Traits::tryDequeue(markQueue)) {
        markHandle.addObject();

        Traits::processInMark(markQueue, top);

        // TODO: Consider moving it before processInMark to make the latter something of a tail call.
        if (auto* extraObjectData = mm::ExtraObjectData::Get(top)) {
            internal::processExtraObjectData<Traits>(markHandle, markQueue, *extraObjectData, top);
        }
    }
}

// Kore: without shadow stack frames, the stack roots of a thread (suspended or native) are the heap objects that any
// word of its stack, or of the registers it saved when it stopped, points into. Collects the candidate words of
// `thread` into `candidates`; nursery objects are roots right away.
template <typename Traits>
void collectConservativeStackCandidates(
        typename Traits::MarkQueue& markQueue, mm::ThreadData& thread, std::vector<ObjHeader**>& candidates) noexcept {
    auto& suspension = thread.suspensionData();
    size_t first = candidates.size();
    auto consider = [&](uintptr_t word) {
        if (word >= 4096) candidates.push_back(reinterpret_cast<ObjHeader**>(word));
        // Kore: with compressed references (docs/kore/gc-redesign.md, phase 3), objects on the stack hold 32-bit references,
        // and compiled code may keep a reference as such or as its byte offset from the base (`ref * 8`) across a call.
        // The heap starts at least 4 GiB above the base, so that 32-bit integers in either form never point into it.
        if (compiler::compressedRefs()) {
            for (uint32_t half : {static_cast<uint32_t>(word), static_cast<uint32_t>(word >> 32)}) {
                if (half != 0) candidates.push_back(reinterpret_cast<ObjHeader**>(decodeRef(half)));
            }
            if (!compiler::compressedRefsZeroBased() && word < kCompressedRefsWindow) {
                candidates.push_back(reinterpret_cast<ObjHeader**>(Kotlin_heapBase + word));
            }
        }
    };
    uintptr_t high = suspension.stackBase();
    for (uintptr_t address = suspension.savedStackPointer() & ~uintptr_t(7); address + sizeof(uintptr_t) <= high;
         address += sizeof(uintptr_t)) {
        consider(*reinterpret_cast<const uintptr_t*>(address));
    }
    for (size_t i = 0; i < mm::ThreadSuspensionData::kSavedRegisters; ++i) consider(suspension.savedRegisters()[i]);
    for (size_t i = first; i < candidates.size(); ++i) {
        auto address = reinterpret_cast<uintptr_t>(candidates[i]);
        ObjHeader* object = alloc::nursery::movableObjectContaining(address);
        if (object == nullptr) object = alloc::nursery::retainedObjectContaining(address);
        if (object != nullptr) {
            if (alloc::nursery::movable(object)) alloc::nursery::pin(object);
            internal::collectRoot<Traits>(markQueue, object);
        }
    }
}

// Kore: the heap objects the candidate words point into are roots. One lookup for many threads' candidates: each
// lookup walks the heap's pages.
template <typename Traits>
void collectConservativeStackRoots(typename Traits::MarkQueue& markQueue, std::vector<ObjHeader**>& candidates) noexcept {
    mm::GlobalData::Instance().allocator().forEachHeapSlot(candidates, [&](ObjHeader**, ObjHeader* object) noexcept {
        internal::collectRoot<Traits>(markQueue, object);
    });
}

template <typename Traits>
void collectRootSetForThread(GCHandle gcHandle, typename Traits::MarkQueue& markQueue, mm::ThreadData& thread,
                             std::vector<ObjHeader**>* stackCandidates = nullptr) {
    auto handle = gcHandle.collectThreadRoots(thread);
    // TODO: Remove useless mm::ThreadRootSet abstraction.
    for (auto value : mm::ThreadRootSet(thread)) {
        if (internal::collectRoot<Traits>(markQueue, value.object)) {
            switch (value.source) {
                case mm::ThreadRootSet::Source::kStack:
                    handle.addStackRoot();
                    break;
                case mm::ThreadRootSet::Source::kTLS:
                    handle.addThreadLocalRoot();
                    break;
            }
        }
    }
    if (compiler::conservativeStacks()) {
        // Kore: with `stackCandidates`, the caller looks the candidates up (for all threads at once).
        if (stackCandidates != nullptr) {
            collectConservativeStackCandidates<Traits>(markQueue, thread, *stackCandidates);
        } else {
            std::vector<ObjHeader**> candidates;
            collectConservativeStackCandidates<Traits>(markQueue, thread, candidates);
            collectConservativeStackRoots<Traits>(markQueue, candidates);
        }
    }
}

template <typename Traits>
void collectRootSetGlobals(GCHandle gcHandle, typename Traits::MarkQueue& markQueue) noexcept {
    auto handle = gcHandle.collectGlobalRoots();
    // TODO: Remove useless mm::GlobalRootSet abstraction.
    for (auto value : mm::GlobalRootSet()) {
        if (internal::collectRoot<Traits>(markQueue, value.object)) {
            switch (value.source) {
                case mm::GlobalRootSet::Source::kGlobal:
                    handle.addGlobalRoot();
                    break;
                case mm::GlobalRootSet::Source::kStableRef:
                    handle.addStableRoot();
                    break;
            }
        }
    }
}

// TODO: This needs some tests now.
template <typename Traits, typename F>
void collectRootSet(GCHandle handle, typename Traits::MarkQueue& markQueue, F&& filter) noexcept {
    Traits::clear(markQueue);
    std::vector<ObjHeader**> stackCandidates;
    for (auto& thread : mm::GlobalData::Instance().threadRegistry().LockForIter()) {
        if (!filter(thread))
            continue;
        thread.Publish();
        collectRootSetForThread<Traits>(handle, markQueue, thread, &stackCandidates);
    }
    if (compiler::conservativeStacks()) collectConservativeStackRoots<Traits>(markQueue, stackCandidates);
    collectRootSetGlobals<Traits>(handle, markQueue);
}

template <typename Traits>
void processWeaks(GCHandle gcHandle, mm::ExternalRCRefRegistry& registry) noexcept {
    auto handle = gcHandle.processWeaks();
    for (auto object : registry.lockForIter()) { // FIXME rename
        auto* obj = object.load(std::memory_order_relaxed);
        if (!obj) {
            // We already processed it at some point.
            handle.addUndisposed();
            continue;
        }
        if (obj->permanent() || Traits::IsMarked(obj)) {
            // TODO: Let's not put permanent objects in here at all?
            // Object is alive. Nothing to do.
            handle.addAlive();
            continue;
        }
        // Object is not alive. Clear it out.
        object.store(nullptr, std::memory_order_relaxed);
        handle.addNulled();
    }
}

struct DefaultProcessWeaksTraits {
    static bool IsMarked(ObjHeader* obj) noexcept { return gc::isMarked(obj); }
};

void stopTheWorld(GCHandle gcHandle, const char* reason) noexcept;
void resumeTheWorld(GCHandle gcHandle) noexcept;
// Kore: with `KORE_GC_PAUSES`, the end of a phase of the current pause (reported with the pause).
void pausePhase(const char* name) noexcept;

[[nodiscard]] inline auto stopTheWorldInScope(GCHandle gcHandle) noexcept {
    return ScopeGuard([=]() { stopTheWorld(gcHandle, "GC stop the world"); }, [=]() { resumeTheWorld(gcHandle); });
}

} // namespace gc
} // namespace kotlin

#endif // RUNTIME_GC_COMMON_MARK_AND_SWEEP_UTILS_H
