// Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by
// the Apache 2.0 license that can be found in the license/LICENSE.txt file.

#include "RecursiveInline.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

using namespace llvm;
using namespace llvm::kotlin;

static cl::opt<unsigned> MaxFunctionSize(
    "kotlin-recursive-inline-max-size", cl::init(400), cl::Hidden,
    cl::desc("The largest function (in instructions) whose recursive calls in "
             "loops are inlined one level deep"));

static cl::opt<unsigned> MaxCallSites(
    "kotlin-recursive-inline-max-calls", cl::init(2), cl::Hidden,
    cl::desc("The most recursive calls in loops a function may have for them "
             "to be inlined"));

namespace {

unsigned instructionCount(const Function &F) {
  unsigned Count = 0;
  for (const BasicBlock &BB : F)
    Count += BB.sizeWithoutDebug();
  return Count;
}

bool eligible(const Function &F) {
  return !F.isDeclaration() && !F.isVarArg() &&
         !F.hasFnAttribute(Attribute::OptimizeNone) &&
         !F.hasFnAttribute(Attribute::NoInline) &&
         instructionCount(F) <= MaxFunctionSize;
}

} // namespace

PreservedAnalyses RecursiveInlinePass::run(Module &M,
                                           ModuleAnalysisManager &AM) {
  auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  SmallVector<Function *> Candidates;
  for (Function &F : M)
    if (eligible(F))
      Candidates.push_back(&F);

  bool Changed = false;
  for (Function *F : Candidates) {
    auto &LI = FAM.getResult<LoopAnalysis>(*F);
    SmallVector<CallBase *> Sites;
    bool Other = false;
    for (BasicBlock &BB : *F) {
      for (Instruction &I : BB) {
        auto *Call = dyn_cast<CallBase>(&I);
        if (!Call || Call->getCalledFunction() != F)
          continue;
        if (Call->isMustTailCall() || Call->isNoInline() ||
            !LI.getLoopFor(&BB)) {
          Other = true;
          continue;
        }
        Sites.push_back(Call);
      }
    }
    // A function with recursive calls outside of loops recurses for its own
    // sake (divide and conquer), where unrolling gains less than it costs.
    if (Sites.empty() || Other || Sites.size() > MaxCallSites)
      continue;

    ValueToValueMapTy VMap;
    Function *Clone = CloneFunction(F, VMap);
    Clone->setLinkage(GlobalValue::PrivateLinkage);
    bool Inlined = false;
    for (CallBase *Call : Sites) {
      Call->setCalledFunction(Clone);
      InlineFunctionInfo IFI;
      if (InlineFunction(*Call, IFI).isSuccess()) {
        Inlined = true;
      } else {
        Call->setCalledFunction(F);
      }
    }
    if (Clone->use_empty())
      Clone->eraseFromParent();
    if (Inlined) {
      FAM.invalidate(*F, PreservedAnalyses::none());
      Changed = true;
    }
  }
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
