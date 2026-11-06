// Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by
// the Apache 2.0 license that can be found in the license/LICENSE.txt file.

#include "LoopSafepoints.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/Process.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <optional>

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

/// Kore: the iterations a call-free loop runs without polling (a few tens of
/// microseconds even for heavy loop bodies). `KORE_LOOP_POLL_CHUNK` overrides
/// it, for experiments.
static uint64_t loopPollChunk() {
  static const uint64_t Chunk = [] {
    if (auto Value = sys::Process::GetEnv("KORE_LOOP_POLL_CHUNK")) {
      uint64_t Parsed = 0;
      if (!StringRef(*Value).getAsInteger(10, Parsed) && Parsed > 1 &&
          Parsed < (uint64_t(1) << 31))
        return Parsed;
    }
    return uint64_t(4096);
  }();
  return Chunk;
}

/// The constant maximum number of times the backedge of the innermost loop
/// `L` is taken, or none if unknown or above 2^32.
static std::optional<uint64_t> maxBackedgeTakenCount(Loop &L,
                                                     ScalarEvolution &SE) {
  if (!L.isInnermost())
    return std::nullopt;
  const SCEV *MaxCount = SE.getConstantMaxBackedgeTakenCount(&L);
  if (isa<SCEVCouldNotCompute>(MaxCount))
    return std::nullopt;
  const APInt &Count = cast<SCEVConstant>(MaxCount)->getAPInt();
  if (Count.getActiveBits() > 32)
    return std::nullopt;
  return Count.getZExtValue();
}

/// Whether `L` calls functions that may run for a while (with loops or calls
/// of their own) without polling: anything but intrinsics, safepoint and frame
/// calls, and calls that do not return (throwing bounds check exceptions).
static bool makesCalls(const Loop &L) {
  for (BasicBlock *BB : L.blocks()) {
    for (Instruction &I : *BB) {
      const auto *Call = dyn_cast<CallBase>(&I);
      if (!Call || isa<IntrinsicInst>(Call) || Call->isInlineAsm() ||
          Call->doesNotReturn())
        continue;
      if (const Function *Callee = Call->getCalledFunction()) {
        StringRef Name = Callee->getName();
        if (Callee->isIntrinsic() || Name == LoopSafepointName ||
            Name == PrologueSafepointName || Name == "EnterFrame" ||
            Name == "LeaveFrame" || Name == "SetCurrentFrame" ||
            Name == "CheckCurrentFrame")
          continue;
      }
      return true;
    }
  }
  return false;
}

/// Marks the polls left for `LoopPollStripMiningPass`.
static constexpr StringLiteral StripMineKind = "kore.strip.mine";

static void inlinePoll(CallBase *Poll) {
  Poll->removeFnAttr(Attribute::NoInline);
  Poll->setMetadata(StripMineKind, nullptr);
  if (!Poll->getCalledFunction()->isDeclaration()) {
    InlineFunctionInfo IFI;
    InlineFunction(*Poll, IFI);
  }
}

PreservedAnalyses LoopSafepointsPass::run(Function &F,
                                          FunctionAnalysisManager &AM) {
  bool Changed = removeInlinedPrologueSafepoints(F);
  if (PrologueOnly)
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  SmallVector<CallBase *> Polls;
  for (Instruction &I : instructions(F)) {
    if (isPendingLoopSafepoint(I) && !I.getMetadata(StripMineKind))
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
    if (!L) {
      ToRemove.push_back(Poll);
      continue;
    }
    // Kore: a loop with calls polls on every iteration (calls do not poll on
    // entry, see `LLVMKotlinRemoveRedundantSafepoints`), a short call-free one
    // does not poll, and any other call-free one (long, or of unknown length,
    // e.g. bounded by a size field that a per-iteration poll would make LLVM
    // reload) polls between chunks of iterations: the chunk counter bounds the
    // time between polls whatever the trip count.
    if (makesCalls(*L)) {
      ToInline.push_back(Poll);
      continue;
    }
    std::optional<uint64_t> MaxCount = maxBackedgeTakenCount(*L, SE);
    if (MaxCount && *MaxCount < loopPollChunk())
      ToRemove.push_back(Poll);
    else
      Poll->setMetadata(StripMineKind, MDNode::get(F.getContext(), {}));
  }
  for (CallBase *Poll : ToRemove)
    Poll->eraseFromParent();
  for (CallBase *Poll : ToInline)
    inlinePoll(Poll);
  return PreservedAnalyses::none();
}

/// Strip-mines `L` (innermost, with a preheader and a latch) with `Poll`
/// between chunks of at most `Chunk` iterations:
///
///   Preheader -> Outer -> Header ... -> Latch -> Header (while the original
///   condition holds and the chunk is not done), Latch -> Chunked -> Exit (if
///   the original condition exits) or -> Poll -> Outer.
///
/// The header's phis take their values from phis of the outer loop, which take
/// the initial values from the preheader and the latch values from the poll.
static bool stripMine(Loop &L, CallBase *Poll, uint64_t Chunk) {
  BasicBlock *Preheader = L.getLoopPreheader();
  BasicBlock *Header = L.getHeader();
  BasicBlock *Latch = L.getLoopLatch();
  if (!Preheader || !Latch)
    return false;
  auto *LatchBr = dyn_cast<BranchInst>(Latch->getTerminator());
  if (!LatchBr)
    return false;
  // The block the latch leaves the loop to, if any.
  BasicBlock *Exit = nullptr;
  bool ContinueOnTrue = true;
  if (LatchBr->isConditional()) {
    BasicBlock *True = LatchBr->getSuccessor(0);
    BasicBlock *False = LatchBr->getSuccessor(1);
    if ((True == Header) == (False == Header))
      return false;
    ContinueOnTrue = True == Header;
    Exit = ContinueOnTrue ? False : True;
    if (L.contains(Exit))
      return false;
  }
  for (PHINode &Phi : Header->phis()) {
    if (Phi.getNumIncomingValues() != 2)
      return false;
  }

  Function &F = *Header->getParent();
  LLVMContext &Ctx = F.getContext();
  auto *Outer =
      BasicBlock::Create(Ctx, Header->getName() + ".chunks", &F, Header);
  auto *Chunked = BasicBlock::Create(Ctx, Latch->getName() + ".chunked", &F);
  auto *PollBlock = BasicBlock::Create(Ctx, Latch->getName() + ".poll", &F);

  // Preheader -> Outer -> Header.
  Preheader->getTerminator()->replaceUsesOfWith(Header, Outer);
  IRBuilder<> B(Outer);
  SmallVector<std::pair<PHINode *, PHINode *>> Phis;
  for (PHINode &Phi : Header->phis()) {
    PHINode *OuterPhi =
        B.CreatePHI(Phi.getType(), 2, Phi.getName() + ".chunks");
    OuterPhi->addIncoming(Phi.getIncomingValueForBlock(Preheader), Preheader);
    OuterPhi->addIncoming(Phi.getIncomingValueForBlock(Latch), PollBlock);
    Phis.push_back({&Phi, OuterPhi});
  }
  B.CreateBr(Header);
  for (auto [Phi, OuterPhi] : Phis) {
    int Index = Phi->getBasicBlockIndex(Preheader);
    Phi->setIncomingBlock(Index, Outer);
    Phi->setIncomingValue(Index, OuterPhi);
  }

  // The iteration count in the chunk.
  Type *CounterTy = Type::getInt32Ty(Ctx);
  PHINode *Counter = PHINode::Create(CounterTy, 2, "chunk.iter");
  Counter->insertBefore(Header->begin());
  Counter->addIncoming(ConstantInt::get(CounterTy, 0), Outer);
  B.SetInsertPoint(LatchBr);
  Value *Next = B.CreateAdd(Counter, ConstantInt::get(CounterTy, 1),
                            "chunk.next", /*HasNUW=*/true, /*HasNSW=*/true);
  Counter->addIncoming(Next, Latch);
  Value *More =
      B.CreateICmpNE(Next, ConstantInt::get(CounterTy, Chunk), "chunk.more");

  // The latch continues while the original condition holds and the chunk is
  // not done; then `Chunked` takes the original exit, or polls.
  if (!LatchBr->isConditional()) {
    BranchInst::Create(Header, Chunked, More, LatchBr->getIterator());
    LatchBr->eraseFromParent();
    BranchInst::Create(PollBlock, Chunked);
  } else {
    Value *Cond = LatchBr->getCondition();
    if (ContinueOnTrue) {
      LatchBr->setCondition(B.CreateAnd(Cond, More));
      LatchBr->setSuccessor(1, Chunked);
      BranchInst::Create(PollBlock, Exit, Cond, Chunked);
    } else {
      LatchBr->setCondition(B.CreateOr(Cond, B.CreateNot(More)));
      LatchBr->setSuccessor(0, Chunked);
      BranchInst::Create(Exit, PollBlock, Cond, Chunked);
    }
    for (PHINode &Phi : Exit->phis()) {
      int Index = Phi.getBasicBlockIndex(Latch);
      if (Index >= 0)
        Phi.setIncomingBlock(Index, Chunked);
    }
  }

  // The poll, between chunks.
  BranchInst::Create(Outer, PollBlock);
  Poll->moveBefore(PollBlock->getTerminator()->getIterator());
  inlinePoll(Poll);
  return true;
}

PreservedAnalyses LoopPollStripMiningPass::run(Function &F,
                                               FunctionAnalysisManager &AM) {
  SmallVector<CallBase *> Polls;
  for (Instruction &I : instructions(F)) {
    if (isPendingLoopSafepoint(I) && I.getMetadata(StripMineKind))
      Polls.push_back(cast<CallBase>(&I));
  }
  if (Polls.empty())
    return PreservedAnalyses::all();
  auto &LI = AM.getResult<LoopAnalysis>(F);
  // One poll per loop. The loops are innermost, hence disjoint: strip-mining
  // one does not change the others.
  SmallPtrSet<Loop *, 8> Seen;
  SmallVector<std::pair<Loop *, CallBase *>> ToStrip;
  SmallVector<CallBase *> ToRemove, ToInline;
  for (CallBase *Poll : Polls) {
    Loop *L = LI.getLoopFor(Poll->getParent());
    if (!L || !L->isInnermost())
      ToInline.push_back(Poll);
    else if (Seen.insert(L).second)
      ToStrip.push_back({L, Poll});
    else
      ToRemove.push_back(Poll);
  }
  for (CallBase *Poll : ToRemove)
    Poll->eraseFromParent();
  for (auto [L, Poll] : ToStrip) {
    if (!stripMine(*L, Poll, loopPollChunk()))
      ToInline.push_back(Poll);
  }
  for (CallBase *Poll : ToInline)
    inlinePoll(Poll);
  return PreservedAnalyses::none();
}
