// Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by
// the Apache 2.0 license that can be found in the license/LICENSE.txt file.

#include "LoopSafepoints.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;
using namespace llvm::kotlin;

static constexpr StringLiteral LoopSafepointName =
    "Kotlin_mm_safePointWhileLoopBody";

static bool isPendingLoopSafepoint(const Instruction &I) {
  const auto *Call = dyn_cast<CallBase>(&I);
  if (!Call || !Call->isNoInline())
    return false;
  const Function *Callee = Call->getCalledFunction();
  return Callee && Callee->getName() == LoopSafepointName;
}


static constexpr StringLiteral PrologueSafepointName =
    "Kotlin_mm_safePointFunctionPrologue";

static bool isPrologueSafepoint(const Instruction &I) {
  const auto *Call = dyn_cast<CallBase>(&I);
  const Function *Callee = Call ? Call->getCalledFunction() : nullptr;
  return Callee && Callee->getName() == PrologueSafepointName;
}

/// The prologue polls of inlined callees are redundant with the function's own
/// (at its entry) and its loops' ones, but as opaque calls they keep LLVM from
/// optimizing across them, so remove them as soon as they are inlined, with
/// the same policy as `LLVMKotlinRemoveRedundantSafepoints`, which runs after
/// optimization: keep the first poll of the entry block, and if there is none,
/// the first poll of every block.
static bool removeInlinedPrologueSafepoints(Function &F) {
  SmallVector<Instruction *> ToRemove;
  bool EntryHasPoll = false;
  for (BasicBlock &BB : F) {
    bool KeepFirst = BB.isEntryBlock() || !EntryHasPoll;
    for (Instruction &I : BB) {
      if (!isPrologueSafepoint(I))
        continue;
      if (KeepFirst) {
        KeepFirst = false;
        if (BB.isEntryBlock())
          EntryHasPoll = true;
      } else {
        ToRemove.push_back(&I);
      }
    }
  }
  for (Instruction *I : ToRemove)
    I->eraseFromParent();
  return !ToRemove.empty();
}

/// Whether `L` runs at most 2^32 times, so that a mutator spends a bounded
/// time in it between safepoints of the enclosing code.
static bool isBoundedLoop(Loop &L, ScalarEvolution &SE) {
  if (!L.isInnermost())
    return false;
  const SCEV *MaxCount = SE.getConstantMaxBackedgeTakenCount(&L);
  if (isa<SCEVCouldNotCompute>(MaxCount))
    return false;
  return cast<SCEVConstant>(MaxCount)->getAPInt().getActiveBits() <= 32;
}

PreservedAnalyses LoopSafepointsPass::run(Function &F,
                                          FunctionAnalysisManager &AM) {
  bool Changed = removeInlinedPrologueSafepoints(F);
  if (PrologueOnly)
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  SmallVector<CallBase *> Polls;
  for (Instruction &I : instructions(F)) {
    if (isPendingLoopSafepoint(I))
      Polls.push_back(cast<CallBase>(&I));
  }
  if (Polls.empty())
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();

  auto &LI = AM.getResult<LoopAnalysis>(F);
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
  SmallVector<CallBase *> ToRemove, ToInline;
  for (CallBase *Poll : Polls) {
    // A poll outside of any loop (e.g. in a fully unrolled loop) is redundant
    // with the function prologue's one.
    Loop *L = LI.getLoopFor(Poll->getParent());
    if (!L || isBoundedLoop(*L, SE))
      ToRemove.push_back(Poll);
    else
      ToInline.push_back(Poll);
  }
  for (CallBase *Poll : ToRemove)
    Poll->eraseFromParent();
  for (CallBase *Poll : ToInline) {
    Poll->removeFnAttr(Attribute::NoInline);
    if (!Poll->getCalledFunction()->isDeclaration()) {
      InlineFunctionInfo IFI;
      InlineFunction(*Poll, IFI);
    }
  }
  return PreservedAnalyses::none();
}
