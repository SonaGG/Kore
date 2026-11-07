// Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by
// the Apache 2.0 license that can be found in the license/LICENSE.txt file.

#include "SmallArrayPromotion.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"

using namespace llvm;

namespace {

constexpr unsigned kMaxElements = 8;

/// A local array the code generator allocated on the stack: its element type, element count and body offset.
struct ArrayShape {
  Type *Element = nullptr;
  unsigned Count = 0;
  uint64_t BodyOffset = 0;
  uint64_t CountOffset = 8; // The 32-bit length after the 8-byte header word.
};

/// `local#IntArray4#internal` and the like: primitive arrays only (their elements hold no references).
bool shapeOf(AllocaInst *Alloca, const DataLayout &DL, ArrayShape &Shape) {
  auto *Struct = dyn_cast<StructType>(Alloca->getAllocatedType());
  if (!Struct || !Struct->hasName()) return false;
  StringRef Name = Struct->getName();
  if (!Name.consume_front("local#")) return false;
  LLVMContext &C = Alloca->getContext();
  struct Kind { const char *Prefix; Type *Element; };
  const Kind Kinds[] = {
      {"IntArray", Type::getInt32Ty(C)},   {"LongArray", Type::getInt64Ty(C)},  {"DoubleArray", Type::getDoubleTy(C)},
      {"FloatArray", Type::getFloatTy(C)}, {"ShortArray", Type::getInt16Ty(C)}, {"CharArray", Type::getInt16Ty(C)},
      {"ByteArray", Type::getInt8Ty(C)},   {"BooleanArray", Type::getInt8Ty(C)},
  };
  for (const Kind &K : Kinds) {
    StringRef Rest = Name;
    if (!Rest.consume_front(K.Prefix)) continue;
    unsigned Count = 0;
    if (Rest.consumeInteger(10, Count) || Count == 0 || Count > kMaxElements) return false;
    uint64_t Size = DL.getTypeAllocSize(Struct);
    uint64_t ElementSize = DL.getTypeStoreSize(K.Element);
    if (Size < uint64_t(Count) * ElementSize + 16) return false;
    Shape.Element = K.Element;
    Shape.Count = Count;
    Shape.BodyOffset = Size - uint64_t(Count) * ElementSize;
    // The body follows the 16-byte array header: anything else is a layout this pass does not know.
    if (Shape.BodyOffset != 16) return false;
    return true;
  }
  return false;
}

/// An access to the array, found by `analyze`.
struct Access {
  enum Kind { HeaderStore, CountLoad, ElementLoad, ElementStore, VectorLoad, Zero, Lifetime } What;
  Instruction *I;
  Value *Index = nullptr;   // ElementLoad/ElementStore: element index (any integer), or null with ConstIndex.
  unsigned ConstIndex = 0;  // Element index when Index is null; first element for VectorLoad/Zero.
  unsigned Elements = 0;    // VectorLoad/Zero: number of elements.
};

/// The accesses of `Alloca`, or false if it is used in another way (passed to a call, stored, compared...).
bool analyze(AllocaInst *Alloca, const ArrayShape &Shape, const DataLayout &DL, SmallVectorImpl<Access> &Accesses,
             SmallVectorImpl<Instruction *> &Addresses) {
  uint64_t ElementSize = DL.getTypeStoreSize(Shape.Element);
  // (pointer, byte offset from the array's start) pairs to visit.
  SmallVector<std::pair<Value *, uint64_t>, 8> Work;
  Work.push_back({Alloca, 0});
  while (!Work.empty()) {
    auto [Pointer, Offset] = Work.pop_back_val();
    for (User *U : Pointer->users()) {
      auto *I = dyn_cast<Instruction>(U);
      if (!I) return false;
      if (auto *GEP = dyn_cast<GetElementPtrInst>(I)) {
        if (GEP->getPointerOperand() != Pointer) return false;
        APInt ConstOffset(DL.getIndexTypeSizeInBits(GEP->getType()), 0);
        if (GEP->accumulateConstantOffset(DL, ConstOffset)) {
          Work.push_back({GEP, Offset + ConstOffset.getZExtValue()});
          Addresses.push_back(GEP);
          continue;
        }
        // An element at a computed index: `gep elem, body, idx` (one index of the element type).
        if (Offset != Shape.BodyOffset || GEP->getNumIndices() != 1 || GEP->getSourceElementType() != Shape.Element) return false;
        Value *Index = GEP->getOperand(1);
        for (User *GU : GEP->users()) {
          if (auto *Load = dyn_cast<LoadInst>(GU)) {
            if (Load->getType() != Shape.Element || Load->isVolatile()) return false;
            Accesses.push_back({Access::ElementLoad, Load, Index});
          } else if (auto *Store = dyn_cast<StoreInst>(GU)) {
            if (Store->getPointerOperand() != GEP || Store->getValueOperand()->getType() != Shape.Element || Store->isVolatile()) return false;
            Accesses.push_back({Access::ElementStore, Store, Index});
          } else {
            return false;
          }
        }
        Addresses.push_back(GEP);
        continue;
      }
      if (auto *II = dyn_cast<IntrinsicInst>(I)) {
        if (II->isLifetimeStartOrEnd()) {
          Accesses.push_back({Access::Lifetime, II});
          continue;
        }
        if (auto *Set = dyn_cast<MemSetInst>(II)) {
          auto *Value = dyn_cast<ConstantInt>(Set->getValue());
          auto *Length = dyn_cast<ConstantInt>(Set->getLength());
          if (Set->getDest() != Pointer || !Value || !Value->isZero() || !Length || Set->isVolatile()) return false;
          uint64_t Bytes = Length->getZExtValue();
          // Zeroing the header (part of a whole-object initialization) is dropped; the body must be whole elements.
          if (Offset < Shape.BodyOffset) {
            if (Offset + Bytes > Shape.BodyOffset && Offset + Bytes != Shape.BodyOffset + Shape.Count * ElementSize) return false;
            uint64_t BodyBytes = Offset + Bytes > Shape.BodyOffset ? Offset + Bytes - Shape.BodyOffset : 0;
            Accesses.push_back({Access::Zero, II, nullptr, 0, unsigned(BodyBytes / ElementSize)});
            continue;
          }
          uint64_t Start = Offset - Shape.BodyOffset;
          if (Start % ElementSize != 0 || Bytes % ElementSize != 0 || Start + Bytes > Shape.Count * ElementSize) return false;
          Accesses.push_back({Access::Zero, II, nullptr, unsigned(Start / ElementSize), unsigned(Bytes / ElementSize)});
          continue;
        }
        return false;
      }
      if (auto *Load = dyn_cast<LoadInst>(I)) {
        if (Load->isVolatile()) return false;
        if (Offset == Shape.CountOffset && Load->getType()->isIntegerTy(32)) {
          Accesses.push_back({Access::CountLoad, Load});
          continue;
        }
        if (Offset < Shape.BodyOffset) return false; // The type info, or the GC bits.
        uint64_t Start = Offset - Shape.BodyOffset;
        if (Start % ElementSize != 0) return false;
        if (Load->getType() == Shape.Element) {
          if (Start / ElementSize >= Shape.Count) return false;
          Accesses.push_back({Access::ElementLoad, Load, nullptr, unsigned(Start / ElementSize)});
          continue;
        }
        auto *Vector = dyn_cast<FixedVectorType>(Load->getType());
        if (!Vector || Vector->getElementType() != Shape.Element || Start / ElementSize + Vector->getNumElements() > Shape.Count) return false;
        Accesses.push_back({Access::VectorLoad, Load, nullptr, unsigned(Start / ElementSize), Vector->getNumElements()});
        continue;
      }
      if (auto *Store = dyn_cast<StoreInst>(I)) {
        if (Store->getPointerOperand() != Pointer || Store->isVolatile()) return false;
        if (Offset < Shape.BodyOffset) {
          // The header (type info, length), which only these accesses could read: dropped.
          if (Offset + DL.getTypeStoreSize(Store->getValueOperand()->getType()) > Shape.BodyOffset) return false;
          Accesses.push_back({Access::HeaderStore, Store});
          continue;
        }
        uint64_t Start = Offset - Shape.BodyOffset;
        if (Store->getValueOperand()->getType() != Shape.Element || Start % ElementSize != 0 || Start / ElementSize >= Shape.Count) return false;
        Accesses.push_back({Access::ElementStore, Store, nullptr, unsigned(Start / ElementSize)});
        continue;
      }
      return false;
    }
  }
  return true;
}

bool promote(AllocaInst *Alloca, const ArrayShape &Shape, const DataLayout &DL, SmallVectorImpl<AllocaInst *> &NewAllocas) {
  SmallVector<Access, 16> Accesses;
  SmallVector<Instruction *, 8> Addresses;
  if (!analyze(Alloca, Shape, DL, Accesses, Addresses)) return false;

  IRBuilder<> Entry(Alloca);
  SmallVector<AllocaInst *, kMaxElements> Slots;
  for (unsigned K = 0; K < Shape.Count; ++K) {
    auto *Slot = Entry.CreateAlloca(Shape.Element, nullptr, "array.slot");
    Slots.push_back(Slot);
    NewAllocas.push_back(Slot);
  }
  Constant *Zero = Constant::getNullValue(Shape.Element);
  // An element index as a 64-bit integer.
  auto IndexAt = [](IRBuilder<> &B, Value *Index) { return B.CreateZExtOrTrunc(Index, B.getInt64Ty()); };
  for (Access &A : Accesses) {
    IRBuilder<> B(A.I);
    switch (A.What) {
      case Access::Lifetime:
      case Access::HeaderStore:
        break;
      case Access::CountLoad:
        A.I->replaceAllUsesWith(B.getInt32(Shape.Count));
        break;
      case Access::Zero:
        for (unsigned K = A.ConstIndex; K < A.ConstIndex + A.Elements; ++K) B.CreateStore(Zero, Slots[K]);
        break;
      case Access::ElementLoad: {
        Value *Result;
        if (!A.Index) {
          Result = B.CreateLoad(Shape.Element, Slots[A.ConstIndex]);
        } else {
          Value *Index = IndexAt(B, A.Index);
          Result = B.CreateLoad(Shape.Element, Slots[0]);
          for (unsigned K = 1; K < Shape.Count; ++K) {
            Value *Element = B.CreateLoad(Shape.Element, Slots[K]);
            Result = B.CreateSelect(B.CreateICmpEQ(Index, B.getInt64(K)), Element, Result);
          }
        }
        A.I->replaceAllUsesWith(Result);
        break;
      }
      case Access::ElementStore: {
        Value *Stored = cast<StoreInst>(A.I)->getValueOperand();
        if (!A.Index) {
          B.CreateStore(Stored, Slots[A.ConstIndex]);
        } else {
          Value *Index = IndexAt(B, A.Index);
          for (unsigned K = 0; K < Shape.Count; ++K) {
            Value *Old = B.CreateLoad(Shape.Element, Slots[K]);
            B.CreateStore(B.CreateSelect(B.CreateICmpEQ(Index, B.getInt64(K)), Stored, Old), Slots[K]);
          }
        }
        break;
      }
      case Access::VectorLoad: {
        Value *Vector = PoisonValue::get(A.I->getType());
        for (unsigned K = 0; K < A.Elements; ++K) {
          Vector = B.CreateInsertElement(Vector, B.CreateLoad(Shape.Element, Slots[A.ConstIndex + K]), B.getInt64(K));
        }
        A.I->replaceAllUsesWith(Vector);
        break;
      }
    }
  }
  for (Access &A : Accesses) A.I->eraseFromParent();
  for (auto It = Addresses.rbegin(); It != Addresses.rend(); ++It) (*It)->eraseFromParent();
  Alloca->eraseFromParent();
  return true;
}

} // namespace

PreservedAnalyses kotlin::SmallArrayPromotionPass::run(Function &F, FunctionAnalysisManager &AM) {
  const DataLayout &DL = F.getDataLayout();
  SmallVector<std::pair<AllocaInst *, ArrayShape>, 4> Candidates;
  for (Instruction &I : F.getEntryBlock()) {
    auto *Alloca = dyn_cast<AllocaInst>(&I);
    ArrayShape Shape;
    if (Alloca && Alloca->isStaticAlloca() && shapeOf(Alloca, DL, Shape)) Candidates.push_back({Alloca, Shape});
  }
  if (Candidates.empty()) return PreservedAnalyses::all();
  SmallVector<AllocaInst *, 16> NewAllocas;
  bool Changed = false;
  for (auto &[Alloca, Shape] : Candidates) Changed |= promote(Alloca, Shape, DL, NewAllocas);
  if (!Changed) return PreservedAnalyses::all();
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  PromoteMemToReg(NewAllocas, DT);
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
