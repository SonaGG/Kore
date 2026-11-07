/*
 * Copyright 2010-2021 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#ifndef RUNTIME_COMPILER_CONSTANTS_H
#define RUNTIME_COMPILER_CONSTANTS_H

#include <cstdint>

#include "Common.h"

// Prefer to use getter functions below. These constants are exposed to simplify the job of the inliner.

/**
 * There are two ways, how compiler can define variables for runtime usage. This one, and the other one with details in source file.
 * On Kotlin code, those variables are defined within [NativeRuntimeConstants.kt].
 *
 * These variables are eligible for runtime optimizations, and fixed at the point of compiling caches.
 * So use this way for variables, which are heavily used on performance-critical passes or will significantly increase code size,
 * if not eliminated.
 *
 * When adding a new variable, please remember to adjust cache-disabling rules and add value to `CompilerGenerated.cpp` for tests.
 */
extern "C" const int32_t Kotlin_needDebugInfo;
extern "C" const int32_t Kotlin_runtimeAssertsMode;
extern "C" const int32_t Kotlin_disableMmap;
extern "C" const int32_t Kotlin_runtimeLogsEnabled;
extern "C" const int32_t Kotlin_concurrentWeakSweep;
extern "C" const int32_t Kotlin_gcMarkSingleThreaded;
extern "C" const int32_t Kotlin_fixedBlockPageSize;
extern "C" const int32_t Kotlin_gcYoungGenerationSize; // Kore
extern "C" const int32_t Kotlin_gcNurserySize; // Kore
extern "C" const int32_t Kotlin_conservativeStacks; // Kore
extern "C" const int32_t Kotlin_immix; // Kore
extern "C" const int32_t Kotlin_compressedRefs; // Kore
extern "C" const int32_t Kotlin_compressedRefsZeroBased; // Kore
extern "C" const int32_t Kotlin_compactHeaders; // Kore
extern "C" const int32_t Kotlin_immixEvacuation; // Kore
extern "C" const int32_t Kotlin_gcWord; // Kore
extern "C" const int32_t Kotlin_pagedAllocator;

class SourceInfo;

namespace kotlin {
namespace compiler {

// Must match RuntimeAssertsMode in RuntimeAssertsMode.kt
enum class RuntimeAssertsMode : int32_t {
    kIgnore = 0,
    kLog = 1,
    kPanic = 2,
};

// Must match AppStateTracking in AppStateTracking.kt
enum class AppStateTracking {
    kDisabled = 0,
    kEnabled = 1,
};

ALWAYS_INLINE inline bool shouldContainDebugInfo() noexcept {
    return Kotlin_needDebugInfo != 0;
}

ALWAYS_INLINE inline RuntimeAssertsMode runtimeAssertsMode() noexcept {
    return static_cast<RuntimeAssertsMode>(Kotlin_runtimeAssertsMode);
}

ALWAYS_INLINE inline bool runtimeAssertsEnabled() noexcept {
    return runtimeAssertsMode() != RuntimeAssertsMode::kIgnore;
}

ALWAYS_INLINE inline bool disableMmap() noexcept {
    return Kotlin_disableMmap != 0;
}

ALWAYS_INLINE inline bool runtimeLogsEnabled() noexcept {
    return Kotlin_runtimeLogsEnabled != 0;
}

ALWAYS_INLINE inline bool concurrentWeakSweep() noexcept {
    return Kotlin_concurrentWeakSweep != 0;
}

ALWAYS_INLINE inline bool gcMarkSingleThreaded() noexcept {
    return Kotlin_gcMarkSingleThreaded != 0;
}

ALWAYS_INLINE inline constexpr int32_t fixedBlockPageSize() noexcept {
    return Kotlin_fixedBlockPageSize;
}

// Kore: young generation size in MiB for the generational GC; 0 selects the runtime default.
ALWAYS_INLINE inline int32_t gcYoungGenerationSize() noexcept {
    return Kotlin_gcYoungGenerationSize;
}

// Kore: generated code keeps no shadow stack frames; stacks are scanned conservatively.
ALWAYS_INLINE inline bool conservativeStacks() noexcept {
    return Kotlin_conservativeStacks != 0;
}

// Kore: size of the moving young generation in MiB; 0 if there is none.
ALWAYS_INLINE inline int32_t gcNurserySize() noexcept {
    return Kotlin_gcNurserySize;
}

// Kore: reference fields of heap objects and `Array` elements are 32-bit (docs/kore/gc-redesign.md, phase 3).
ALWAYS_INLINE inline bool compressedRefs() noexcept {
    return Kotlin_compressedRefs != 0;
}

// Kore: compressed references are the low 32 bits of the address (the heap and the image lie below 4 GiB).
ALWAYS_INLINE inline bool compressedRefsZeroBased() noexcept {
    return Kotlin_compressedRefsZeroBased != 0;
}

// Kore: the header word of an object (not an array) holds a 32-bit type info or meta object address (with the tag and GC
// bits) in its low half, and the first 4-byte field of the object in its high half (docs/kore/compressed-refs.md).
ALWAYS_INLINE inline bool compactHeaders() noexcept {
    return Kotlin_compactHeaders != 0;
}

// Kore: young collections copy survivors out of Immix blocks (docs/kore/gc-redesign.md, phase 5).
ALWAYS_INLINE inline bool immixEvacuation() noexcept {
    return Kotlin_immixEvacuation != 0;
}

// Kore: small and medium objects live in Immix blocks (alloc/custom ImmixBlock.hpp).
ALWAYS_INLINE inline bool immix() noexcept {
    return Kotlin_immix != 0;
}

// Kore: whether heap objects get a GC word before them (gc/gen ObjectData.hpp). Only the generational GC with the
// custom allocator, Immix and no nursery can do without it; the compiler keeps it in other configurations.
ALWAYS_INLINE inline bool gcWord() noexcept {
    return Kotlin_gcWord != 0;
}
ALWAYS_INLINE inline bool pagedAllocator() noexcept {
    return Kotlin_pagedAllocator != 0;
}


bool gcMutatorsCooperate() noexcept;
uint32_t auxGCThreads() noexcept;
uint32_t concurrentMarkMaxIterations() noexcept;
bool suspendFunctionsFromAnyThreadFromObjCEnabled() noexcept;
AppStateTracking appStateTracking() noexcept;
int getSourceInfo(void* addr, SourceInfo *result, int result_size) noexcept;
bool coreSymbolicationUseOnlyKotlinImage() noexcept;
bool objcDisposeOnMain() noexcept;
bool objcDisposeWithRunLoop() noexcept;
bool enableSafepointSignposts() noexcept;
bool globalDataLazyInit() noexcept;
bool swiftExport() noexcept;
bool latin1Strings() noexcept;
uint8_t mmapTag() noexcept;
const char* minidumpLocation() noexcept;
bool minidumpOnSIGTERM() noexcept;
const int32_t* runtimeLogs() noexcept;

#ifdef KONAN_ANDROID
bool printToAndroidLogcat() noexcept;
#endif

} // namespace compiler
} // namespace kotlin

#endif // RUNTIME_COMPILER_CONSTANTS_H
