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
/// that run at most `LoopPollChunk` times, and inlines the polls of loops that
/// make calls (a poll costs little next to a call, and the callee may not
/// poll). The polls of the other innermost loops that run at most 2^32 times
/// are left for `LoopPollStripMiningPass`, which moves them out of the loop,
/// so that the loop can still be vectorized and have its invariants hoisted.
/// The remaining polls are inlined. It also removes the function prologue polls
/// of inlined callees.
class LoopSafepointsPass : public PassInfoMixin<LoopSafepointsPass> {
public:
  /// With `PrologueOnly`, only removes the prologue polls of inlined callees,
  /// for use before loops are simplified.
  explicit LoopSafepointsPass(bool PrologueOnly = false)
      : PrologueOnly(PrologueOnly) {}

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

private:
  bool PrologueOnly;
};

/// Kore: strip-mines the call-free innermost loops `LoopSafepointsPass` left a
/// poll in: the loop runs in chunks of at most `LoopPollChunk` iterations, with
/// the poll between chunks (in an outer loop), like the loop strip mining of
/// HotSpot's C2. Runs after IRCE, which needs the original latch condition.
class LoopPollStripMiningPass : public PassInfoMixin<LoopPollStripMiningPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm::kotlin
