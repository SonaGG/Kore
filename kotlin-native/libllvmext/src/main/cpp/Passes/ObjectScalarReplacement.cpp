// Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by
// the Apache 2.0 license that can be found in the license/LICENSE.txt file.

#include "ObjectScalarReplacement.h"
#include "Frames.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"

#include <map>
#include <optional>

using namespace llvm;
using namespace llvm::kotlin;

static constexpr StringLiteral AllocInstanceName = "AllocInstance";
static constexpr StringLiteral UpdateStackRefName = "UpdateStackRef";
// Frames start with `FrameOverlay` (previous frame, parameter and slot counts).
static constexpr int64_t FrameHeaderSize = 16;

static bool isCallTo(const Value *V, StringRef Name) {
  const auto *Call = dyn_cast<CallInst>(V);
  const Function *Callee = Call ? Call->getCalledFunction() : nullptr;
  return Callee && Callee->getName() == Name;
}

/// A heap allocation that the code generator kept a call for this pass.
static bool isPendingAllocation(const Value *V) {
  return isCallTo(V, AllocInstanceName) && cast<CallInst>(V)->isNoInline() &&
         isa<Constant>(cast<CallInst>(V)->getArgOperand(0));
}

/// An object allocated on the stack by the code generator.
static bool isStackObject(const Value *V) {
  const auto *Alloca = dyn_cast<AllocaInst>(V);
  if (!Alloca || Alloca->isArrayAllocation() || !Alloca->isStaticAlloca())
    return false;
  const auto *Type = dyn_cast<StructType>(Alloca->getAllocatedType());
  return Type && Type->hasName() && Type->getName().starts_with("kclassbody:");
}

static bool isFrame(const AllocaInst &Alloca) {
  return Alloca.getName().starts_with("kore.frame");
}

namespace {

/// The slots of the frames of a function (including the frames of inlined
/// functions), by frame and byte offset.
class FrameSlots {
public:
  explicit FrameSlots(Function &F) : DL(F.getDataLayout()) {
    for (Instruction &I : instructions(F)) {
      if (auto *Alloca = dyn_cast<AllocaInst>(&I); Alloca && isFrame(*Alloca))
        analyze(*Alloca);
    }
  }

  /// Promotes the loads from slots that only this function writes to SSA
  /// values.
  bool promote(Function &F) {
    bool Changed = false;
    for (auto &[Key, Slot] : Slots) {
      if (Slot.Unpromotable || Slot.Loads.empty())
        continue;
      Changed |= promote(F, Slot);
    }
    return Changed;
  }

  /// Whether `Store` stores to a slot that is never read (or whose reads were
  /// promoted), so that the stored value is only seen by the garbage collector.
  bool isWriteOnlySlot(const Value *Address) const {
    std::optional<std::pair<const AllocaInst *, int64_t>> Key = slotOf(Address);
    if (!Key)
      return false;
    auto It = Slots.find(*Key);
    return It != Slots.end() && !It->second.Unpromotable &&
           It->second.Loads.empty();
  }

  bool isWriteOnlyStackRefUpdate(const CallInst &Call) const {
    return isCallTo(&Call, UpdateStackRefName) &&
           isWriteOnlySlot(Call.getArgOperand(0));
  }

private:
  struct Slot {
    SmallVector<LoadInst *> Loads;
    // Stores, `UpdateStackRef` calls, allocations with this result slot, and
    // memsets of the frame.
    SmallVector<Instruction *> Writes;
    bool Unpromotable = false;
  };

  const DataLayout &DL;
  DenseSet<const AllocaInst *> Frames;
  std::map<std::pair<const AllocaInst *, int64_t>, Slot> Slots;
  SmallVector<std::tuple<const AllocaInst *, Instruction *, int64_t, int64_t>>
      Clears;

  std::optional<std::pair<const AllocaInst *, int64_t>>
  slotOf(const Value *Address) const {
    APInt Offset(DL.getIndexTypeSizeInBits(Address->getType()), 0);
    const Value *Base =
        Address->stripAndAccumulateConstantOffsets(DL, Offset, true);
    const auto *Alloca = dyn_cast<AllocaInst>(Base);
    if (!Alloca || !Frames.contains(Alloca) ||
        Offset.getSExtValue() < FrameHeaderSize)
      return std::nullopt;
    return std::make_pair(Alloca, Offset.getSExtValue());
  }

  void analyze(AllocaInst &Frame) {
    SmallVector<std::pair<Instruction *, int64_t>> Accesses;
    SmallVector<std::pair<Value *, int64_t>> Worklist = {{&Frame, 0}};
    DenseSet<Value *> Visited;
    auto Fail = [&] {
      Frames.erase(&Frame);
      for (auto It = Slots.begin(); It != Slots.end();) {
        if (It->first.first == &Frame)
          It = Slots.erase(It);
        else
          ++It;
      }
    };
    Frames.insert(&Frame);
    while (!Worklist.empty()) {
      auto [Pointer, Offset] = Worklist.pop_back_val();
      if (!Visited.insert(Pointer).second)
        continue;
      for (User *U : Pointer->users()) {
        auto *I = dyn_cast<Instruction>(U);
        if (!I)
          return Fail();
        if (auto *GEP = dyn_cast<GetElementPtrInst>(I)) {
          APInt GEPOffset(DL.getIndexTypeSizeInBits(GEP->getType()), 0);
          if (!GEP->accumulateConstantOffset(DL, GEPOffset))
            return Fail();
          Worklist.push_back({GEP, Offset + GEPOffset.getSExtValue()});
          continue;
        }
        if (auto *Phi = dyn_cast<PHINode>(I)) {
          // The code generator's `slotsPhi`.
          if (Phi->hasConstantValue() != Pointer)
            return Fail();
          Worklist.push_back({Phi, Offset});
          continue;
        }
        bool IsSlot = Offset >= FrameHeaderSize;
        auto Key = std::make_pair(static_cast<const AllocaInst *>(&Frame), Offset);
        if (auto *Load = dyn_cast<LoadInst>(I)) {
          if (IsSlot) {
            Slot &S = Slots[Key];
            if (Load->isSimple() && Load->getType()->isPointerTy())
              S.Loads.push_back(Load);
            else
              S.Unpromotable = true;
          }
          continue;
        }
        if (auto *Store = dyn_cast<StoreInst>(I)) {
          if (Store->getPointerOperand() == Pointer) {
            if (IsSlot) {
              Slot &S = Slots[Key];
              if (Store->isSimple() &&
                  Store->getValueOperand()->getType()->isPointerTy())
                S.Writes.push_back(Store);
              else
                S.Unpromotable = true;
            }
          } else if (Offset != 0) {
            // The address of a slot escapes.
            if (!IsSlot)
              return Fail();
            Slots[Key].Unpromotable = true;
          }
          // Otherwise the frame is registered with the thread (`EnterFrame`).
          continue;
        }
        if (auto *MemSet = dyn_cast<MemSetInst>(I)) {
          auto *Length = dyn_cast<ConstantInt>(MemSet->getLength());
          if (MemSet->getDest() != Pointer || !Length ||
              !isa<ConstantInt>(MemSet->getValue()) ||
              !cast<ConstantInt>(MemSet->getValue())->isZero())
            return Fail();
          Clears.push_back({&Frame, MemSet, Offset,
                            Offset + static_cast<int64_t>(Length->getZExtValue())});
          continue;
        }
        if (auto *Intrinsic = dyn_cast<IntrinsicInst>(I);
            Intrinsic && Intrinsic->isLifetimeStartOrEnd())
          continue;
        if (auto *Call = dyn_cast<CallInst>(I)) {
          if (IsSlot && isPendingAllocation(Call) &&
              Call->getArgOperand(1) == Pointer &&
              Call->getArgOperand(0) != Pointer) {
            Slots[Key].Writes.push_back(Call);
            continue;
          }
          if (IsSlot && isCallTo(Call, UpdateStackRefName) &&
              Call->getArgOperand(0) == Pointer &&
              Call->getArgOperand(1) != Pointer) {
            Slots[Key].Writes.push_back(Call);
            continue;
          }
          if (Offset == 0 && Call->getCalledFunction()) {
            StringRef Name = Call->getCalledFunction()->getName();
            if (Name == "EnterFrame" || Name == "LeaveFrame" ||
                Name == "SetCurrentFrame" || Name == "CheckCurrentFrame")
              continue;
          }
          // A callee may write the slot (e.g. a result slot).
          if (!IsSlot)
            return Fail();
          Slots[Key].Unpromotable = true;
          continue;
        }
        return Fail();
      }
    }
    // Clears write null to every slot they cover.
    for (auto &[ClearFrame, Clear, Begin, End] : Clears) {
      if (ClearFrame != &Frame)
        continue;
      for (auto &[Key, S] : Slots) {
        if (Key.first == &Frame && Key.second >= Begin &&
            Key.second + 8 <= End)
          S.Writes.push_back(Clear);
      }
    }
  }

  static Value *writtenValue(Instruction *Write) {
    if (auto *Store = dyn_cast<StoreInst>(Write))
      return Store->getValueOperand();
    if (isCallTo(Write, UpdateStackRefName))
      return cast<CallInst>(Write)->getArgOperand(1);
    if (isa<MemSetInst>(Write))
      return ConstantPointerNull::get(PointerType::get(Write->getContext(), 0));
    return Write; // The allocation.
  }

  bool promote(Function &F, Slot &S) {
    DenseSet<Instruction *> Writes(S.Writes.begin(), S.Writes.end());
    DenseSet<Instruction *> Loads(S.Loads.begin(), S.Loads.end());
    SetVector<BasicBlock *> Blocks;
    for (Instruction *I : S.Writes)
      Blocks.insert(I->getParent());
    for (Instruction *I : S.Loads)
      Blocks.insert(I->getParent());

    Type *PtrTy = PointerType::get(F.getContext(), 0);
    SSAUpdater SSA;
    SSA.Initialize(PtrTy, "kore.slot");
    BasicBlock &Entry = F.getEntryBlock();
    // The memory of a frame of an inlined function is undefined until the
    // frame is cleared: null is as good a value as any.
    bool EntryWritten = false;
    SmallVector<LoadInst *> Pending;
    SmallVector<std::pair<LoadInst *, Value *>> Local;
    for (BasicBlock *BB : Blocks) {
      Value *Current = nullptr;
      for (Instruction &I : *BB) {
        if (Writes.contains(&I)) {
          Current = writtenValue(&I);
        } else if (auto *Load = dyn_cast<LoadInst>(&I);
                   Load && Loads.contains(Load)) {
          if (Current)
            Local.push_back({Load, Current});
          else
            Pending.push_back(Load);
        }
      }
      if (Current) {
        SSA.AddAvailableValue(BB, Current);
        EntryWritten |= BB == &Entry;
      }
    }
    if (!EntryWritten)
      SSA.AddAvailableValue(&Entry, ConstantPointerNull::get(cast<PointerType>(PtrTy)));
    // Values first, as replacing loads may change the blocks' values.
    SmallVector<std::pair<LoadInst *, Value *>> Replacements(Local);
    for (LoadInst *Load : Pending) {
      if (Load->getParent() == &Entry) {
        // Before any write in the entry block: the frame is not set up yet.
        Replacements.push_back(
            {Load, ConstantPointerNull::get(cast<PointerType>(PtrTy))});
        continue;
      }
      Replacements.push_back({Load, SSA.GetValueInMiddleOfBlock(Load->getParent())});
    }
    // A load may be the written value of a later write (`x = x`): resolve
    // chains of replaced loads.
    DenseMap<Value *, Value *> Replaced;
    for (auto &[Load, NewValue] : Replacements)
      Replaced[Load] = NewValue;
    for (auto &[Load, V] : Replacements) {
      Value *Resolved = V;
      for (unsigned Steps = 0; Replaced.count(Resolved) && Steps < Replacements.size(); ++Steps)
        Resolved = Replaced[Resolved];
      Load->replaceAllUsesWith(Resolved);
    }
    for (auto &[Load, V] : Replacements)
      Load->eraseFromParent();
    S.Loads.clear();
    return !Replacements.empty();
  }
};

/// A field (or the header) of an object, or whether a reference is null.
struct FieldKey {
  int64_t Offset;
  Type *Ty; // Null for the null flag.
  bool operator==(const FieldKey &Other) const {
    return Offset == Other.Offset && Ty == Other.Ty;
  }
};

/// A web of objects (allocations and stack objects, "sources") and of phis and
/// selects of them ("merges") that can be replaced by field values.
class ObjectWeb {
public:
  ObjectWeb(Function &F, DominatorTree &DT, const FrameSlots &Slots)
      : F(F), DT(DT), Slots(Slots), DL(F.getDataLayout()) {}

  bool reject(int Line, const Instruction *At = nullptr) {
    if (sys::Process::GetEnv("KORE_OSR_DEBUG")) {
      errs() << "kore-osr: " << F.getName() << ": rejected at line " << Line;
      if (At)
        errs() << ": " << *At;
      errs() << '\n';
    }
    return false;
  }

  bool build(Instruction *Start) {
    SmallVector<Value *> Worklist = {Start};
    while (!Worklist.empty()) {
      Value *V = Worklist.pop_back_val();
      if (isa<ConstantPointerNull>(V))
        continue;
      auto *I = dyn_cast<Instruction>(V);
      if (!I || !Members.insert(I))
        continue;
      if (isPendingAllocation(I) || isStackObject(I)) {
        Sources.insert(I);
      } else if (auto *Phi = dyn_cast<PHINode>(I)) {
        for (Value *Incoming : Phi->incoming_values())
          Worklist.push_back(Incoming);
      } else if (auto *Select = dyn_cast<SelectInst>(I)) {
        Worklist.push_back(Select->getTrueValue());
        Worklist.push_back(Select->getFalseValue());
      } else {
        return reject(__LINE__);
      }
      if (Members.size() > MaxMembers)
        return reject(__LINE__);
      if (!collectUses(I, Worklist))
        return reject(__LINE__);
    }
    for (Instruction *Source : Sources) {
      if (!findInitBlock(Source))
        return reject(__LINE__);
    }
    return checkValues();
  }

  void replace() {
    // Merged field values: create the phis first (they may be cyclic).
    for (Instruction *Member : Members) {
      auto *Phi = dyn_cast<PHINode>(Member);
      if (!Phi)
        continue;
      for (const FieldKey &Key : Keys) {
        Type *Ty = Key.Ty ? Key.Ty : Type::getInt1Ty(F.getContext());
        PHINode *FieldPhi = PHINode::Create(Ty, Phi->getNumIncomingValues(),
                                            "kore.field", Phi->getIterator());
        MergedValues[{Member, keyIndex(Key)}] = FieldPhi;
      }
    }
    for (Instruction *Member : Members) {
      auto *Phi = dyn_cast<PHINode>(Member);
      if (!Phi)
        continue;
      for (const FieldKey &Key : Keys) {
        auto *FieldPhi = cast<PHINode>(MergedValues[{Member, keyIndex(Key)}]);
        for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
          BasicBlock *Pred = Phi->getIncomingBlock(I);
          FieldPhi->addIncoming(
              *valueOf(Phi->getIncomingValue(I), Key, Pred, nullptr, true), Pred);
        }
      }
    }
    for (auto &[Load, Base, Offset] : Reads) {
      Value *V = *valueOf(Base, {Offset, Load->getType()}, Load->getParent(), Load, true);
      Load->replaceAllUsesWith(V);
    }
    for (auto &[Compare, Base] : NullChecks) {
      Value *IsNull = *valueOf(Base, {0, nullptr}, Compare->getParent(), Compare, true);
      if (Compare->getPredicate() == CmpInst::ICMP_NE)
        IsNull = BinaryOperator::CreateNot(IsNull, "kore.nonnull", Compare->getIterator());
      Compare->replaceAllUsesWith(IsNull);
    }
    for (ICmpInst *Compare : IdentityChecks)
      Compare->replaceAllUsesWith(ConstantInt::getBool(
          Compare->getType(), Compare->getPredicate() == CmpInst::ICMP_NE));
    // The garbage collector does not need to see the objects any more.
    for (auto &[Write, Operand] : SlotWrites)
      Write->setOperand(Operand, ConstantPointerNull::get(
                                     cast<PointerType>(Write->getOperand(Operand)->getType())));

    SmallVector<Instruction *> Dead;
    for (auto &[Load, Base, Offset] : Reads)
      Dead.push_back(Load);
    for (auto &[Compare, Base] : NullChecks)
      Dead.push_back(Compare);
    for (ICmpInst *Compare : IdentityChecks)
      Dead.push_back(Compare);
    for (auto &[Source, Writes] : SourceWrites) {
      if (isPendingAllocation(Source)) {
        for (auto &W : Writes) {
          if (W.Inst != Source)
            Dead.push_back(W.Inst);
        }
      }
    }
    for (Instruction *I : Dead) {
      I->replaceAllUsesWith(PoisonValue::get(I->getType()));
      I->eraseFromParent();
    }
    for (Instruction *Member : Members) {
      if (!Sources.contains(Member))
        Member->replaceAllUsesWith(PoisonValue::get(Member->getType()));
    }
    for (Instruction *Member : Members) {
      if (!Sources.contains(Member))
        Member->eraseFromParent();
    }
    for (Instruction *Source : Sources) {
      if (!isPendingAllocation(Source))
        continue;
      SmallVector<Instruction *> Users;
      for (User *U : Source->users())
        Users.push_back(cast<Instruction>(U));
      for (Instruction *U : Users)
        RecursivelyDeleteTriviallyDeadInstructions(U);
      if (Source->use_empty())
        Source->eraseFromParent();
    }
  }

  const SetVector<Instruction *> &members() const { return Members; }

private:
  static constexpr unsigned MaxMembers = 64;

  struct Write {
    Instruction *Inst;
    int64_t Offset;
    int64_t Size;
    Value *Val; // Null for zeroes.
  };

  Function &F;
  DominatorTree &DT;
  const FrameSlots &Slots;
  const DataLayout &DL;
  SetVector<Instruction *> Members;
  SetVector<Instruction *> Sources;
  SmallVector<std::tuple<LoadInst *, Instruction *, int64_t>> Reads;
  SmallVector<std::pair<ICmpInst *, Instruction *>> NullChecks;
  SmallVector<ICmpInst *> IdentityChecks;
  SmallVector<std::pair<Instruction *, unsigned>> SlotWrites;
  DenseMap<Instruction *, SmallVector<Write>> SourceWrites;
  DenseMap<Instruction *, BasicBlock *> InitBlocks;
  DenseMap<Instruction *, SmallVector<BasicBlock *>> Chains;
  std::map<std::pair<BasicBlock *, BasicBlock *>, SmallPtrSet<BasicBlock *, 16>>
      ReachableAvoiding;
  SmallVector<FieldKey> Keys;
  std::map<std::pair<Instruction *, unsigned>, Value *> MergedValues;

  unsigned keyIndex(const FieldKey &Key) {
    for (unsigned I = 0; I < Keys.size(); ++I) {
      if (Keys[I] == Key)
        return I;
    }
    Keys.push_back(Key);
    return Keys.size() - 1;
  }

  bool collectUses(Instruction *Member, SmallVectorImpl<Value *> &Worklist) {
    bool IsSource = Sources.contains(Member);
    SmallVector<std::pair<Value *, int64_t>> Pointers = {{Member, 0}};
    while (!Pointers.empty()) {
      auto [Pointer, Offset] = Pointers.pop_back_val();
      for (User *U : Pointer->users()) {
        auto *I = dyn_cast<Instruction>(U);
        if (!I)
          return reject(__LINE__);
        if (auto *GEP = dyn_cast<GetElementPtrInst>(I)) {
          APInt GEPOffset(DL.getIndexTypeSizeInBits(GEP->getType()), 0);
          if (!GEP->accumulateConstantOffset(DL, GEPOffset))
            return reject(__LINE__);
          Pointers.push_back({GEP, Offset + GEPOffset.getSExtValue()});
          continue;
        }
        if (auto *Load = dyn_cast<LoadInst>(I)) {
          // A reference read from the object would only be held in registers,
          // where the garbage collector does not see it.
          if (!Load->isSimple() ||
              (Offset != 0 && Load->getType()->isPtrOrPtrVectorTy()) ||
              Load->getType()->isAggregateType())
            return reject(__LINE__, I);
          Reads.push_back({Load, Member, Offset});
          keyIndex({Offset, Load->getType()});
          continue;
        }
        if (auto *Store = dyn_cast<StoreInst>(I)) {
          if (Store->getPointerOperand() == Pointer &&
              Store->getValueOperand() != Pointer) {
            if (!IsSource || !Store->isSimple())
              return reject(__LINE__, I);
            SourceWrites[Member].push_back(
                {Store, Offset,
                 static_cast<int64_t>(DL.getTypeStoreSize(Store->getValueOperand()->getType())),
                 Store->getValueOperand()});
            continue;
          }
          // The object itself stored to a frame slot.
          if (Pointer != Member || Store->getPointerOperand() == Pointer ||
              !Slots.isWriteOnlySlot(Store->getPointerOperand()))
            return reject(__LINE__, I);
          SlotWrites.push_back({Store, 0});
          continue;
        }
        if (auto *MemSet = dyn_cast<MemSetInst>(I)) {
          auto *Length = dyn_cast<ConstantInt>(MemSet->getLength());
          auto *Byte = dyn_cast<ConstantInt>(MemSet->getValue());
          if (!IsSource || MemSet->getDest() != Pointer || !Length || !Byte ||
              !Byte->isZero() || MemSet->isVolatile())
            return reject(__LINE__, I);
          SourceWrites[Member].push_back(
              {MemSet, Offset, static_cast<int64_t>(Length->getZExtValue()), nullptr});
          continue;
        }
        if (auto *Intrinsic = dyn_cast<IntrinsicInst>(I);
            Intrinsic && Intrinsic->isLifetimeStartOrEnd() && IsSource)
          continue;
        if (auto *Compare = dyn_cast<ICmpInst>(I)) {
          if (Pointer != Member || !Compare->isEquality())
            return reject(__LINE__, I);
          Value *Other = Compare->getOperand(Compare->getOperand(0) == Member ? 1 : 0);
          if (isa<ConstantPointerNull>(Other)) {
            NullChecks.push_back({Compare, Member});
            keyIndex({0, nullptr});
          } else {
            // The objects of the web are not seen anywhere else: a reference
            // from outside of the web is never one of them.
            IdentityChecks.push_back(Compare);
          }
          continue;
        }
        if (auto *Call = dyn_cast<CallInst>(I)) {
          if (Pointer == Member && Slots.isWriteOnlyStackRefUpdate(*Call) &&
              Call->getArgOperand(1) == Member &&
              Call->getArgOperand(0) != Member) {
            SlotWrites.push_back({Call, 1});
            continue;
          }
          return reject(__LINE__, I);
        }
        if (isa<PHINode>(I) || isa<SelectInst>(I)) {
          if (Pointer != Member)
            return reject(__LINE__, I);
          if (auto *Select = dyn_cast<SelectInst>(I);
              Select && Select->getCondition() == Member)
            return reject(__LINE__, I);
          Worklist.push_back(I);
          continue;
        }
        return reject(__LINE__, I);
      }
    }
    return true;
  }

  /// All writes to an allocation must follow it in its block (so that a use
  /// of the allocation sees them all). Writes to stack objects are checked per
  /// use in `sourceValue`.
  bool findInitBlock(Instruction *Source) {
    if (!isPendingAllocation(Source))
      return true;
    for (const Write &W : SourceWrites[Source]) {
      if (W.Inst->getParent() != Source->getParent() ||
          !Source->comesBefore(W.Inst))
        return reject(__LINE__, W.Inst);
    }
    return true;
  }

  /// Whether `To` can be reached from the end of `From` without passing
  /// through `Avoid`.
  bool reachesAvoiding(BasicBlock *From, BasicBlock *To, BasicBlock *Avoid) {
    auto &Reachable = ReachableAvoiding[{From, Avoid}];
    if (Reachable.empty()) {
      Reachable.insert(nullptr); // Computed.
      SmallVector<BasicBlock *> Worklist(successors(From));
      while (!Worklist.empty()) {
        BasicBlock *BB = Worklist.pop_back_val();
        if (BB == Avoid || !Reachable.insert(BB).second)
          continue;
        append_range(Worklist, successors(BB));
      }
    }
    return Reachable.contains(To);
  }

  /// The value of `Key` of a source at `Before` in `BB` (at its end if null).
  std::optional<Value *> sourceValue(Instruction *Source, const FieldKey &Key,
                                     BasicBlock *BB, Instruction *Before) {
    if (!Key.Ty) // The null flag.
      return ConstantInt::getFalse(F.getContext());
    int64_t Size = DL.getTypeStoreSize(Key.Ty);
    auto Overlaps = [&](const Write &W) {
      return W.Offset < Key.Offset + Size && Key.Offset < W.Offset + W.Size;
    };
    // The last write to the field in `Block` before `Limit`.
    auto LastWrite = [&](BasicBlock *Block, Instruction *Limit) -> const Write * {
      const Write *Last = nullptr;
      for (const Write &W : SourceWrites[Source]) {
        if (W.Inst->getParent() != Block ||
            (Limit && !W.Inst->comesBefore(Limit)) || !Overlaps(W))
          continue;
        if (!Last || Last->Inst->comesBefore(W.Inst))
          Last = &W;
      }
      return Last;
    };
    auto ValueOf = [&](const Write &W) -> std::optional<Value *> {
      if (!W.Val) {
        if (W.Offset <= Key.Offset && Key.Offset + Size <= W.Offset + W.Size)
          return Constant::getNullValue(Key.Ty);
        return std::nullopt;
      }
      if (W.Offset == Key.Offset && W.Val->getType() == Key.Ty)
        return W.Val;
      return std::nullopt;
    };
    if (isPendingAllocation(Source)) {
      if (const Write *W = LastWrite(Source->getParent(),
                                     BB == Source->getParent() ? Before : nullptr))
        return ValueOf(*W);
      // The allocator clears objects and sets their type info.
      if (Key.Offset == 0 && Key.Ty->isPointerTy())
        return cast<CallInst>(Source)->getArgOperand(0);
      return Constant::getNullValue(Key.Ty);
    }
    // A stack object: the last write to the field in `BB` before the use...
    if (const Write *W = LastWrite(BB, Before))
      return ValueOf(*W);
    // ...or else that of the nearest dominating block that writes it, if no
    // other write to the field can run between that block and the use.
    if (!DT.getNode(BB))
      return std::nullopt;
    BasicBlock *Dominating = nullptr;
    for (DomTreeNode *N = DT.getNode(BB)->getIDom(); N; N = N->getIDom()) {
      if (LastWrite(N->getBlock(), nullptr)) {
        Dominating = N->getBlock();
        break;
      }
    }
    if (!Dominating)
      return std::nullopt; // Uninitialized stack memory.
    for (const Write &W : SourceWrites[Source]) {
      BasicBlock *Block = W.Inst->getParent();
      if (Block == Dominating || !Overlaps(W))
        continue;
      // A later write in the use's block, which may run again before the use.
      if (Block == BB) {
        if (reachesAvoiding(BB, BB, Dominating))
          return std::nullopt;
        continue;
      }
      if (reachesAvoiding(Block, BB, Dominating))
        return std::nullopt;
    }
    return ValueOf(*LastWrite(Dominating, nullptr));
  }

  /// The value of `Key` of the web member (or null) `V` at `Before` in `BB`.
  std::optional<Value *> valueOf(Value *V, const FieldKey &Key, BasicBlock *BB,
                                 Instruction *Before, bool Materialize) {
    if (isa<ConstantPointerNull>(V)) {
      if (!Key.Ty)
        return ConstantInt::getTrue(F.getContext());
      return Constant::getNullValue(Key.Ty);
    }
    auto *I = cast<Instruction>(V);
    if (Sources.contains(I))
      return sourceValue(I, Key, BB, Before);
    if (!Materialize)
      return PoisonValue::get(Key.Ty ? Key.Ty : Type::getInt1Ty(F.getContext()));
    auto Index = std::make_pair(I, keyIndex(Key));
    if (auto It = MergedValues.find(Index); It != MergedValues.end())
      return It->second;
    auto *Select = cast<SelectInst>(I);
    Value *True = *valueOf(Select->getTrueValue(), Key, Select->getParent(), Select, true);
    Value *False = *valueOf(Select->getFalseValue(), Key, Select->getParent(), Select, true);
    Value *Result = SelectInst::Create(Select->getCondition(), True, False,
                                       "kore.field", Select->getIterator());
    MergedValues[Index] = Result;
    return Result;
  }

  /// Whether every value the replacement needs is known.
  bool checkValues() {
    for (auto &[Load, Base, Offset] : Reads) {
      if (!valueOf(Base, {Offset, Load->getType()}, Load->getParent(), Load, false))
        return reject(__LINE__, Load);
    }
    for (auto &[Compare, Base] : NullChecks) {
      if (!valueOf(Base, {0, nullptr}, Compare->getParent(), Compare, false))
        return reject(__LINE__);
    }
    for (ICmpInst *Compare : IdentityChecks) {
      for (Value *Operand : Compare->operands()) {
        if (!Members.contains(dyn_cast<Instruction>(getUnderlyingObject(Operand))) &&
            !isa<Instruction>(Operand) && !isa<Argument>(Operand) && !isa<Constant>(Operand))
          return reject(__LINE__, Compare);
      }
      if (Members.contains(dyn_cast<Instruction>(getUnderlyingObject(Compare->getOperand(0)))) ==
          Members.contains(dyn_cast<Instruction>(getUnderlyingObject(Compare->getOperand(1)))))
        return reject(__LINE__, Compare);
    }
    for (Instruction *Member : Members) {
      SmallVector<std::tuple<Value *, BasicBlock *, Instruction *>> Inputs;
      if (auto *Phi = dyn_cast<PHINode>(Member)) {
        for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I)
          Inputs.push_back({Phi->getIncomingValue(I), Phi->getIncomingBlock(I), nullptr});
      } else if (auto *Select = dyn_cast<SelectInst>(Member)) {
        Inputs.push_back({Select->getTrueValue(), Select->getParent(), Select});
        Inputs.push_back({Select->getFalseValue(), Select->getParent(), Select});
      }
      for (auto &[Input, BB, Before] : Inputs) {
        for (const FieldKey &Key : Keys) {
          if (!valueOf(Input, Key, BB, Before, false)) {
            if (sys::Process::GetEnv("KORE_OSR_DEBUG")) {
              errs() << "  input " << *Input << " from " << BB->getName() << "\n";
              if (auto *Src = dyn_cast<Instruction>(Input); Src && Sources.contains(Src)) {
                errs() << "  init " << InitBlocks[Src]->getName() << " chain";
                for (BasicBlock *C : Chains[Src]) errs() << " " << C->getName();
                errs() << "\n";
                for (const Write &W : SourceWrites[Src]) errs() << "   w " << W.Offset << " " << W.Inst->getParent()->getName() << ": " << *W.Inst << "\n";
              }
            }
            return reject(__LINE__, Member);
          }
        }
      }
    }
    return true;
  }
};

} // namespace

PreservedAnalyses ObjectScalarReplacementPass::run(Function &F,
                                                   FunctionAnalysisManager &AM) {
  bool Changed = false;
  {
    FrameSlots Slots(F);
    Changed |= Slots.promote(F);
  }
  // Promotion turned reads of slots into SSA values: find which slots are now
  // only written.
  FrameSlots Slots(F);
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  SmallVector<Instruction *> Candidates;
  for (Instruction &I : instructions(F)) {
    if (isPendingAllocation(&I) || isStackObject(&I))
      Candidates.push_back(&I);
  }
  DenseSet<Instruction *> Done;
  for (Instruction *Candidate : Candidates) {
    if (Done.contains(Candidate))
      continue;
    ObjectWeb Web(F, DT, Slots);
    bool Replaceable = Web.build(Candidate);
    for (Instruction *Member : Web.members())
      Done.insert(Member);
    if (!Replaceable)
      continue;
    Web.replace();
    Changed = true;
  }
  if (!Changed)
    return PreservedAnalyses::all();
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}

PreservedAnalyses InlinePendingAllocationsPass::run(Module &M,
                                                    ModuleAnalysisManager &) {
  // Kore: keep the body of the function prologue safepoint until the code
  // generator inlines it after the LTO pipeline (which would drop it otherwise,
  // leaving a call in every function).
  for (StringRef Name : {"Kotlin_mm_safePointFunctionPrologue", "EnterFrame", "LeaveFrame"}) {
    Function *Kept = M.getFunction(Name);
    if (!Kept || Kept->isDeclaration())
      continue;
    SmallVector<GlobalValue *> Used;
    collectUsedGlobalVariables(M, Used, /*CompilerUsed=*/false);
    if (!is_contained(Used, Kept))
      appendToUsed(M, {Kept});
  }
  // Kore: link GC frames only where needed, now that inlining is done and
  // allocations are still recognizable calls (the rest of the pipeline
  // optimizes the result).
  shrinkWrapFrames(M, /*Cleanup=*/false);
  Function *Allocate = M.getFunction(AllocInstanceName);
  if (!Allocate)
    return PreservedAnalyses::all();
  SmallVector<CallInst *> Calls;
  for (User *U : Allocate->users()) {
    if (auto *Call = dyn_cast<CallInst>(U);
        Call && Call->getCalledFunction() == Allocate && Call->isNoInline())
      Calls.push_back(Call);
  }
  if (Calls.empty())
    return PreservedAnalyses::all();
  for (CallInst *Call : Calls) {
    Call->removeFnAttr(Attribute::NoInline);
    if (!Allocate->isDeclaration() && !Allocate->isInterposable()) {
      InlineFunctionInfo IFI;
      InlineFunction(*Call, IFI);
    }
  }
  return PreservedAnalyses::none();
}
