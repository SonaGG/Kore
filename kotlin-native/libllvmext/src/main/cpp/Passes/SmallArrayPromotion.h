// Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by
// the Apache 2.0 license that can be found in the license/LICENSE.txt file.

#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm::kotlin {

/// Kore: promotes small primitive arrays allocated on the stack (`local#IntArray4` and the like) to registers when
/// their elements are only read and written, also at computed indexes: an access at a computed index becomes a select
/// over the elements. Without it, such an array stays in memory, and code that writes elements and then reads them
/// together (a vector load) stalls on the stores.
class SmallArrayPromotionPass : public PassInfoMixin<SmallArrayPromotionPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm::kotlin
