// Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by
// the Apache 2.0 license that can be found in the license/LICENSE.txt file.

#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm::kotlin {

/// Kore: decides which loops keep their safepoint polls.
///
/// The code generator emits a `noinline` call to the loop safepoint function at
/// the start of every loop body, so that the call stays recognizable while the
/// function is simplified. This pass removes the polls from innermost loops
/// that run at most 2^32 times (like the counted-loop rule of the JVMs'
/// compilers and LLVM's PlaceSafepoints), so that they can be vectorized and
/// have their invariants hoisted, and inlines the remaining polls.
class LoopSafepointsPass : public PassInfoMixin<LoopSafepointsPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm::kotlin
