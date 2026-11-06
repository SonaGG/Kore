// Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by
// the Apache 2.0 license that can be found in the license/LICENSE.txt file.

#include "UncheckedFunctions.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Instructions.h"

using namespace llvm;
using namespace llvm::kotlin;

static bool isCheckFailureCall(const Instruction &I) {
  const auto *Call = dyn_cast<CallBase>(&I);
  const Function *Callee = Call ? Call->getCalledFunction() : nullptr;
  if (!Callee)
    return false;
  StringRef Name = Callee->getName();
  return Name == "ThrowArrayIndexOutOfBoundsException" ||
         Name == "ThrowNullPointerException";
}

/// A block that only fails a check: it calls one of the throwing functions and
/// does not return (it ends with `unreachable`, or with an `invoke` of the
/// throwing function inside a `try`).
static bool isCheckFailureBlock(const BasicBlock &BB) {
  const Instruction *Terminator = BB.getTerminator();
  if (const auto *Invoke = dyn_cast<InvokeInst>(Terminator))
    return isCheckFailureCall(*Invoke);
  if (!isa<UnreachableInst>(Terminator))
    return false;
  for (const Instruction &I : BB) {
    if (isCheckFailureCall(I))
      return true;
  }
  return false;
}

PreservedAnalyses UncheckedFunctionsPass::run(Function &F,
                                              FunctionAnalysisManager &AM) {
  if (!F.hasFnAttribute("kore-unchecked"))
    return PreservedAnalyses::all();
  SmallVector<BranchInst *> Checks;
  for (BasicBlock &BB : F) {
    auto *Br = dyn_cast<BranchInst>(BB.getTerminator());
    if (!Br || !Br->isConditional())
      continue;
    bool FailsOnTrue = isCheckFailureBlock(*Br->getSuccessor(0));
    bool FailsOnFalse = isCheckFailureBlock(*Br->getSuccessor(1));
    if (FailsOnTrue != FailsOnFalse)
      Checks.push_back(Br);
  }
  for (BranchInst *Br : Checks) {
    bool FailsOnTrue = isCheckFailureBlock(*Br->getSuccessor(0));
    BasicBlock *Failure = Br->getSuccessor(FailsOnTrue ? 0 : 1);
    BasicBlock *Other = Br->getSuccessor(FailsOnTrue ? 1 : 0);
    BasicBlock *BB = Br->getParent();
    Failure->removePredecessor(BB);
    BranchInst::Create(Other, Br->getIterator());
    Br->eraseFromParent();
  }
  return Checks.empty() ? PreservedAnalyses::all() : PreservedAnalyses::none();
}
