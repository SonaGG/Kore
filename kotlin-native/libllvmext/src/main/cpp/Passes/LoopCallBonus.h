// Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by
// the Apache 2.0 license that can be found in the license/LICENSE.txt file.

#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm::kotlin {

/// Kore: static hotness for the inliner. Without a profile, LLVM treats every
/// call site alike, while the JVMs' compilers inline what runs often: mostly
/// the calls in loops. This pass gives calls in loops an inlining threshold
/// bonus (`call-threshold-bonus`) growing with the loop depth, except for calls
/// in blocks that cannot return normally (exception paths).
class LoopCallBonusPass : public PassInfoMixin<LoopCallBonusPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm::kotlin
