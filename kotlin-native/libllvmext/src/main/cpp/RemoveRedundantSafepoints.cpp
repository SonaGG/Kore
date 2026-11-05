// Copyright 2010-2026 JetBrains s.r.o. and Kotlin Programming Language
// contributors. Use of this source code is governed by the Apache 2.0 license
// that can be found in the license/LICENSE.txt file.

#include "RemoveRedundantSafepoints.h"
#include "Passes/Frames.h"

#include "CAPIExtensions.h"

#include "llvm-c/Core.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <cstring>
#include <string>

using namespace llvm;

static constexpr const char *FunctionPrologueSafepointName =
    "Kotlin_mm_safePointFunctionPrologue";

static constexpr size_t FunctionPrologueSafepointNameLength =
    std::char_traits<char>::length(FunctionPrologueSafepointName);

static bool isAPrologueSafepoint(LLVMValueRef Inst) {
  if (LLVMIsACallInst(Inst) || LLVMIsAInvokeInst(Inst)) {
    size_t CalledNameLength = 0;
    const char *CalledName =
        LLVMGetValueName2(LLVMGetCalledValue(Inst), &CalledNameLength);
    return FunctionPrologueSafepointNameLength == CalledNameLength &&
           strncmp(CalledName, FunctionPrologueSafepointName,
                   CalledNameLength) == 0;
  }
  return false;
}

static bool containsPrologueSafepoint(LLVMBasicBlockRef BB) {
  for (auto Inst = LLVMGetFirstInstruction(BB); Inst;
       Inst = LLVMGetNextInstruction(Inst)) {
    if (isAPrologueSafepoint(Inst))
      return true;
  }
  return false;
}

static void removeOrInlinePrologueSafepointInstructions(
    LLVMBasicBlockRef BB, bool RemoveFirst, bool IsSafepointInliningAllowed) {
  // Collect safepoint calls to erase and inline.
  LLVMValueRef ToInline = nullptr;
  SmallVector<LLVMValueRef> ToErase;
  bool First = true;
  for (auto Inst = LLVMGetFirstInstruction(BB); Inst;
       Inst = LLVMGetNextInstruction(Inst)) {
    if (isAPrologueSafepoint(Inst)) {
      if (!First || RemoveFirst) {
        ToErase.push_back(Inst);
      } else if (IsSafepointInliningAllowed &&
                 !LLVMIsDeclaration(LLVMGetCalledValue(Inst))) {
        ToInline = Inst;
      }
      First = false;
    }
  }
  for (auto Inst : ToErase) {
    LLVMInstructionEraseFromParent(Inst);
  }
  if (ToInline) {
    LLVMKotlinInlineCall(ToInline);
  }
}

// Kore: the functions that need a prologue safepoint (like HotSpot, which polls
// in loops and on returns, not on entry). Loops poll on their own, so a
// function needs to poll only if it may call itself again before returning:
// if it is in a cycle of direct calls, or makes indirect (virtual) calls. Any
// other function reaches a poll of a callee or of a loop, or returns, after a
// bounded number of steps, which bounds the time to reach a safepoint.
static DenseSet<const Function *> functionsNeedingPrologueSafepoints(Module &M) {
  DenseSet<const Function *> Result;
  CallGraph CG(M);
  for (auto It = scc_begin(&CG); !It.isAtEnd(); ++It) {
    const std::vector<CallGraphNode *> &SCC = *It;
    bool Cyclic = SCC.size() > 1 || It.hasCycle();
    for (CallGraphNode *Node : SCC) {
      const Function *F = Node->getFunction();
      if (!F || F->isDeclaration())
        continue;
      bool Needs = Cyclic;
      for (const Instruction &I : instructions(*F)) {
        const auto *Call = dyn_cast<CallBase>(&I);
        if (Call && !Call->getCalledFunction() && !Call->isInlineAsm()) {
          Needs = true;
          break;
        }
      }
      if (Needs)
        Result.insert(F);
    }
  }
  return Result;
}

void LLVMKotlinRemoveRedundantSafepoints(LLVMModuleRef M,
                                         int IsSafepointInliningAllowed) {
  DenseSet<const Function *> NeedsSafepoint;
  if (IsSafepointInliningAllowed)
    NeedsSafepoint = functionsNeedingPrologueSafepoints(*unwrap(M));
  for (auto F = LLVMGetFirstFunction(M); F; F = LLVMGetNextFunction(F)) {
    if (LLVMIsDeclaration(F))
      continue;
    LLVMBasicBlockRef FirstBB = LLVMGetFirstBasicBlock(F);
    if (!FirstBB)
      continue;
    bool FirstBBHasSafepoint = containsPrologueSafepoint(FirstBB);
    bool RemoveAll = IsSafepointInliningAllowed && !NeedsSafepoint.contains(unwrap<Function>(F));
    removeOrInlinePrologueSafepointInstructions(FirstBB, /*RemoveFirst=*/RemoveAll,
                                                IsSafepointInliningAllowed);
    for (auto BB = LLVMGetNextBasicBlock(FirstBB); BB;
         BB = LLVMGetNextBasicBlock(BB)) {
      removeOrInlinePrologueSafepointInstructions(BB, FirstBBHasSafepoint,
                                                  IsSafepointInliningAllowed);
    }
  }
  // Kore: after the safepoints, which are calls the frames must be linked for.
  llvm::kotlin::shrinkWrapFrames(*unwrap(M), /*Cleanup=*/true);
}
