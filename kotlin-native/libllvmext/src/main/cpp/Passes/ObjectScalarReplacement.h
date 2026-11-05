// Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by
// the Apache 2.0 license that can be found in the license/LICENSE.txt file.

#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm::kotlin {

/// Kore: scalar replacement of Kotlin objects, including objects merged by
/// phis (like HotSpot C2's "reduce allocation merges" and Graal's partial
/// escape analysis).
///
/// First, local variables kept in the GC frame ("kore.frame" allocas) are
/// promoted to SSA values: Kore's garbage collector does not move objects, and
/// only the function itself writes the slots that it does not pass to calls,
/// so a load from such a slot yields the value last stored there. The stores
/// are kept, for the garbage collector.
///
/// Then, objects allocated by the function (heap allocations, which the code
/// generator keeps as `noinline` calls to `AllocInstance` until this pass has
/// run, and stack allocations) that are only read from, compared with null and
/// stored to frame slots are replaced by the values of their fields: the
/// allocation disappears. Field values flow through phis of such objects, so
/// e.g. `var p = Point(0, 0); for (...) p = p + d` keeps `p.x` and `p.y` in
/// registers. Objects with reference fields that are read are kept, because
/// the garbage collector would not see the referenced objects.
class ObjectScalarReplacementPass
    : public PassInfoMixin<ObjectScalarReplacementPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

/// Kore: inlines the allocation calls that `ObjectScalarReplacementPass` kept.
class InlinePendingAllocationsPass
    : public PassInfoMixin<InlinePendingAllocationsPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm::kotlin
