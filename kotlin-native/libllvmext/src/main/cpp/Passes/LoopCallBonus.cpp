// Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by
// the Apache 2.0 license that can be found in the license/LICENSE.txt file.

#include "LoopCallBonus.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Instructions.h"

#include <string>

using namespace llvm;
using namespace llvm::kotlin;

// The bonus per loop level, and its maximum.
static constexpr unsigned BonusPerDepth = 100;
static constexpr unsigned MaxBonus = 250;

static bool endsInUnreachable(const BasicBlock &BB) {
  return isa<UnreachableInst>(BB.getTerminator());
}

PreservedAnalyses LoopCallBonusPass::run(Function &F,
                                         FunctionAnalysisManager &AM) {
  auto &LI = AM.getResult<LoopAnalysis>(F);
  if (LI.empty())
    return PreservedAnalyses::all();
  bool Changed = false;
  for (BasicBlock &BB : F) {
    unsigned Depth = LI.getLoopDepth(&BB);
    if (Depth == 0 || endsInUnreachable(BB))
      continue;
    std::string Bonus = std::to_string(std::min(MaxBonus, Depth * BonusPerDepth));
    for (Instruction &I : BB) {
      auto *Call = dyn_cast<CallBase>(&I);
      if (!Call || Call->isNoInline())
        continue;
      Function *Callee = Call->getCalledFunction();
      if (!Callee || Callee == &F || Callee->isDeclaration() ||
          Callee->hasFnAttribute(Attribute::Cold) ||
          Callee->hasFnAttribute(Attribute::NoInline) ||
          Callee->getName().starts_with("Kotlin_mm_"))
        continue;
      Call->addFnAttr(Attribute::get(F.getContext(), "call-threshold-bonus", Bonus));
      Changed = true;
    }
  }
  return Changed ? PreservedAnalyses::allInSet<CFGAnalyses>() : PreservedAnalyses::all();
}
