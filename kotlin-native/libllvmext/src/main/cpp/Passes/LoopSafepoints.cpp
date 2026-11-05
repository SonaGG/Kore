// Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by
// the Apache 2.0 license that can be found in the license/LICENSE.txt file.

#include "LoopSafepoints.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/IRBuilder.h"
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


/// Whether `Load` reads an array length: Kore's runtime gives array lengths a
/// type of their own in type-based alias analysis (`ArrayLengthRep`).
static bool isArrayLengthLoad(const LoadInst &Load) {
  const MDNode *Tag = Load.getMetadata(LLVMContext::MD_tbaa);
  if (!Tag || Tag->getNumOperands() < 2)
    return false;
  const auto *AccessType = dyn_cast<MDNode>(Tag->getOperand(1));
  if (!AccessType || AccessType->getNumOperands() < 1)
    return false;
  const auto *Name = dyn_cast<MDString>(AccessType->getOperand(0));
  return Name && Name->getString() == "_ZTS14ArrayLengthRep";
}

/// Whether `L` inspects the type of `Object` (loads its type info, at offset 0,
/// or passes it to a runtime type check), so that a length load in `L` may be
/// guarded by a type check and `Object` may not be an array everywhere.
static bool checksTypeIn(const Loop &L, const Value *Object) {
  for (const BasicBlock *BB : L.blocks()) {
    for (const Instruction &I : *BB) {
      if (const auto *Load = dyn_cast<LoadInst>(&I)) {
        if (Load->getPointerOperand() == Object)
          return true;
      } else if (const auto *Call = dyn_cast<CallBase>(&I)) {
        const Function *Callee = Call->getCalledFunction();
        if (Callee && Callee->getName().starts_with("IsInstance") &&
            is_contained(Call->args(), Object))
          return true;
        if (!Callee && is_contained(Call->args(), Object))
          return true;
      }
    }
  }
  return false;
}

/// Loads the lengths of arrays used in loops once, before the outermost loop
/// in which the array is invariant, instead of at every bounds check. Kotlin
/// arrays never change length, and a non-null array reference always points to
/// a valid array; for a null one (whose length load would crash anyway) the
/// load reads 0 instead, without a branch. Loop-invariant lengths let range
/// check elimination split loops into ones without bounds checks, which can
/// then be vectorized.
static bool hoistArrayLengths(Function &F, LoopInfo &LI) {
  const DataLayout &DL = F.getDataLayout();
  SmallVector<LoadInst *> Loads;
  for (Instruction &I : instructions(F)) {
    auto *Load = dyn_cast<LoadInst>(&I);
    if (Load && Load->isSimple() && LI.getLoopFor(Load->getParent()) &&
        isArrayLengthLoad(*Load))
      Loads.push_back(Load);
  }
  if (Loads.empty())
    return false;
  GlobalVariable *Zero = nullptr;
  // One hoisted length per (array, offset, preheader).
  DenseMap<std::tuple<Value *, int64_t, BasicBlock *>, Value *> Hoisted;
  bool Changed = false;
  for (LoadInst *Load : Loads) {
    APInt Offset(DL.getIndexTypeSizeInBits(Load->getPointerOperandType()), 0);
    Value *Array = Load->getPointerOperand()->stripAndAccumulateInBoundsConstantOffsets(DL, Offset);
    // The outermost loop in which the array is invariant.
    Loop *Target = nullptr;
    for (Loop *L = LI.getLoopFor(Load->getParent()); L && L->isLoopInvariant(Array) && L->getLoopPreheader() &&
                    !checksTypeIn(*L, Array);
         L = L->getParentLoop())
      Target = L;
    if (!Target)
      continue;
    BasicBlock *Preheader = Target->getLoopPreheader();
    auto Key = std::make_tuple(Array, Offset.getSExtValue(), Preheader);
    Value *Length = Hoisted.lookup(Key);
    if (!Length) {
      if (!Zero) {
        Zero = new GlobalVariable(*F.getParent(), Load->getType(), /*isConstant=*/true,
                                  GlobalValue::PrivateLinkage, Constant::getNullValue(Load->getType()),
                                  "kore.zero.length");
        Zero->setAlignment(Load->getAlign());
      }
      IRBuilder<> Builder(Preheader->getTerminator());
      Value *Address = Builder.CreateInBoundsPtrAdd(Array, Builder.getInt(Offset));
      Value *IsNull = Builder.CreateIsNull(Array);
      Value *Safe = Builder.CreateSelect(IsNull, Zero, Address);
      auto *Hoist = Builder.CreateAlignedLoad(Load->getType(), Safe, Load->getAlign(), "array.length");
      Hoist->copyMetadata(*Load, {LLVMContext::MD_tbaa, LLVMContext::MD_range});
      Hoisted[Key] = Length = Hoist;
    }
    Load->replaceAllUsesWith(Length);
    Load->eraseFromParent();
    Changed = true;
  }
  return Changed;
}

PreservedAnalyses LoopSafepointsPass::run(Function &F,
                                          FunctionAnalysisManager &AM) {
  bool Changed = removeInlinedPrologueSafepoints(F);
  if (PrologueOnly)
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  Changed |= hoistArrayLengths(F, AM.getResult<LoopAnalysis>(F));
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
