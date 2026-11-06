// Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by
// the Apache 2.0 license that can be found in the license/LICENSE.txt file.

#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm::kotlin {

/// Kore: removes the array index and null checks of functions the code
/// generator marked `kore-unchecked` (Kotlin functions annotated with
/// `@kore.annotations.Unchecked`): a conditional branch to a block that only
/// throws `ArrayIndexOutOfBoundsException` or `NullPointerException` becomes an
/// unconditional branch to its other successor. Runs after inlining into the
/// function, so it covers the checks of inlined callees (such as the array
/// accessors of the runtime) too.
class UncheckedFunctionsPass : public PassInfoMixin<UncheckedFunctionsPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm::kotlin
