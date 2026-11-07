// Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by
// the Apache 2.0 license that can be found in the license/LICENSE.txt file.

#include "ArrayLengthRange.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Metadata.h"

using namespace llvm;
using namespace llvm::kotlin;

namespace {

// The access type of a struct-path TBAA tag is its second operand, a type node
// whose first operand is the type's name.
bool isArrayLengthAccess(const MDNode *Tag) {
  if (!Tag || Tag->getNumOperands() < 2)
    return false;
  auto *Access = dyn_cast_or_null<MDNode>(Tag->getOperand(1));
  if (!Access || Access->getNumOperands() < 1)
    return false;
  auto *Name = dyn_cast_or_null<MDString>(Access->getOperand(0));
  return Name && Name->getString() == "_ZTS14ArrayLengthRep";
}

// `StringHeader::flags_` (see KString.h), read through its own type
// `StringFlagsRep`. Its encoding bits never change; the only bit written later
// (`HASHCODE_IS_ZERO`) makes a stale read compute the same hash code again.
bool isStringFlagsAccess(const MDNode *Tag) {
  if (!Tag || Tag->getNumOperands() < 2)
    return false;
  auto *Access = dyn_cast_or_null<MDNode>(Tag->getOperand(1));
  if (!Access || Access->getNumOperands() < 1)
    return false;
  auto *Name = dyn_cast_or_null<MDString>(Access->getOperand(0));
  return Name && Name->getString() == "_ZTS14StringFlagsRep";
}

} // namespace

PreservedAnalyses ArrayLengthRangePass::run(Function &F,
                                            FunctionAnalysisManager &) {
  MDNode *Range = nullptr;
  bool Changed = false;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *Load = dyn_cast<LoadInst>(&I);
      if (Load && Load->getType()->isIntegerTy(16) && !Load->isVolatile() &&
          Load->isUnordered() &&
          !Load->hasMetadata(LLVMContext::MD_invariant_load) &&
          isStringFlagsAccess(Load->getMetadata(LLVMContext::MD_tbaa))) {
        Load->setMetadata(LLVMContext::MD_invariant_load,
                          MDNode::get(F.getContext(), {}));
        Changed = true;
        continue;
      }
      if (!Load || !Load->getType()->isIntegerTy(32) || Load->isVolatile() ||
          !Load->isUnordered() ||
          !isArrayLengthAccess(Load->getMetadata(LLVMContext::MD_tbaa)))
        continue;
      if (!Range)
        Range = MDBuilder(F.getContext())
                    .createRange(APInt(32, 0), APInt(32, 0x80000000u));
      if (!Load->hasMetadata(LLVMContext::MD_range))
        Load->setMetadata(LLVMContext::MD_range, Range);
      // Kore: an array's length never changes while code holds a reference to
      // the array, so its load can be hoisted out of loops even past calls
      // (such as safepoint polls): then IRCE removes the bounds checks of the
      // loop and the vectorizer can work on it.
      if (!Load->hasMetadata(LLVMContext::MD_invariant_load))
        Load->setMetadata(LLVMContext::MD_invariant_load,
                          MDNode::get(F.getContext(), {}));
      Changed = true;
    }
  }
  if (!Changed)
    return PreservedAnalyses::all();
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
