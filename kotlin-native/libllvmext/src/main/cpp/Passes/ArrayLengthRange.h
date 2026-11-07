// Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by
// the Apache 2.0 license that can be found in the license/LICENSE.txt file.

#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm::kotlin {

/// Kore: array lengths never exceed Int.MAX_VALUE. This pass gives the loads of
/// array lengths (`ArrayHeader::count_`, which the runtime reads through its own
/// type-based alias analysis type, `ArrayLengthRep`) the range [0, 2^31). An
/// index that passed the bounds check (`index <u length`) is then known to be
/// non-negative, so its sign extension to a 64-bit address becomes a zero
/// extension, which x86-64 32-bit loads and operations do for free. Range
/// metadata, unlike `llvm.assume`, adds no instructions for the optimizer to
/// work around.
class ArrayLengthRangePass : public PassInfoMixin<ArrayLengthRangePass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm::kotlin
