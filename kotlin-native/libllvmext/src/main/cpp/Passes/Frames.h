// Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by
// the Apache 2.0 license that can be found in the license/LICENSE.txt file.

#pragma once

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

namespace llvm::kotlin {

/// Kore: keeps the calls that link GC frames into the thread's shadow stack
/// (`EnterFrame`, `LeaveFrame`) recognizable until `shrinkWrapFrames`, with
/// memory effects limited to the frame and the thread's state, so that they
/// do not stand in the way of other optimizations.
class PendingFramesPass : public PassInfoMixin<PendingFramesPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

/// Kore: after all inlining, links GC frames only where the garbage collector
/// may see them, i.e. around calls (the only places where it may run or scan
/// the stack: allocation slow paths, safepoints, other functions and native
/// code):
///  - a frame with no call while it is linked is not linked at all;
///  - a frame with calls only outside of loops and without exception handlers
///    is linked around each of its calls, so that paths without calls (fast
///    paths) do not link it;
///  - other frames are linked in the prologue, as before.
/// Then inlines the frame calls. Returns whether anything changed.
bool shrinkWrapFrames(Module &M, bool Cleanup);

/// Kore: `shrinkWrapFrames` for one function, once its callees are inlined.
class ShrinkWrapFramesPass : public PassInfoMixin<ShrinkWrapFramesPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm::kotlin
