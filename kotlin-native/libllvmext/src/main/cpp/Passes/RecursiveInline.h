// Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by
// the Apache 2.0 license that can be found in the license/LICENSE.txt file.

#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm::kotlin {

/// Kore: inlines self-recursive calls made in loops one level deep, as the JVMs'
/// compilers do (HotSpot's `MaxRecursiveInlineLevel`). LLVM's inliner never
/// inlines a function into itself, so a recursive walk (a tree traversal, an
/// interpreter running nested blocks) pays a full call, with its prologue and
/// epilogue, for every step down. Inlining the call once makes every other level
/// run in its caller's frame, and lets the optimizer keep values in registers
/// across that level.
///
/// The function is cloned, its recursive calls are pointed at the clone and the
/// clone is inlined: the calls the inlined body makes still go to the original
/// function, so the recursion is unrolled exactly once. Only small functions
/// with few such calls qualify, to bound the code growth.
class RecursiveInlinePass : public PassInfoMixin<RecursiveInlinePass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm::kotlin
