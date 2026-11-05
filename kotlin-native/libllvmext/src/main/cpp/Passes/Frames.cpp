// Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by
// the Apache 2.0 license that can be found in the license/LICENSE.txt file.

#include "Frames.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/ModRef.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;
using namespace llvm::kotlin;

static constexpr StringLiteral EnterFrameName = "EnterFrame";
static constexpr StringLiteral LeaveFrameName = "LeaveFrame";
static constexpr StringLiteral SwitchToNativeName = "Kotlin_mm_switchThreadStateNative";
static constexpr StringLiteral SwitchToRunnableName = "Kotlin_mm_switchThreadStateRunnable";

static StringRef calleeName(const Instruction &I) {
  const auto *Call = dyn_cast<CallBase>(&I);
  const Function *Callee = Call ? Call->getCalledFunction() : nullptr;
  return Callee ? Callee->getName() : StringRef();
}

static bool isFrameCall(const Instruction &I) {
  StringRef Name = calleeName(I);
  return Name == EnterFrameName || Name == LeaveFrameName ||
         Name == "SetCurrentFrame" || Name == "CheckCurrentFrame";
}

/// Where the garbage collector may run, or scan this thread's stack.
static bool isGCPoint(const Instruction &I) {
  const auto *Call = dyn_cast<CallBase>(&I);
  if (!Call || isFrameCall(I))
    return false;
  return !isa<IntrinsicInst>(Call);
}

static const AllocaInst *frameOf(const CallBase &Call) {
  return dyn_cast<AllocaInst>(getUnderlyingObject(Call.getArgOperand(0)));
}

PreservedAnalyses PendingFramesPass::run(Module &M, ModuleAnalysisManager &) {
  bool Changed = false;
  for (StringRef Name : {StringRef(EnterFrameName), StringRef(LeaveFrameName),
                         StringRef(SwitchToNativeName), StringRef(SwitchToRunnableName)}) {
    Function *F = M.getFunction(Name);
    if (!F)
      continue;
    // Keep the signature (argument promotion would pass `previous` instead of
    // the frame) and the body, for inlining once the frames are placed.
    if (!F->isDeclaration()) {
      SmallVector<GlobalValue *> Used;
      collectUsedGlobalVariables(M, Used, /*CompilerUsed=*/false);
      if (!is_contained(Used, F))
        appendToUsed(M, {F});
    }
    for (User *U : F->users()) {
      auto *Call = dyn_cast<CallInst>(U);
      if (!Call || Call->getCalledFunction() != F)
        continue;
      Call->addFnAttr(Attribute::NoInline);
      // Each call must stay attributable to its frame.
      Call->addFnAttr(Attribute::NoMerge);
      if (Name == EnterFrameName || Name == LeaveFrameName)
        Call->setMemoryEffects(MemoryEffects::argMemOnly() |
                               MemoryEffects::inaccessibleMemOnly());
      Changed = true;
    }
  }
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

namespace {

struct FrameRegion {
  SmallVector<Instruction *> GCPoints;
  bool HasInvoke = false;
  bool Leaks = false; // Reaches the end of the function without leaving the frame.
  // Switches to the native state: the GC scans the stack concurrently with the
  // native code, so the frame must stay linked across the whole native call.
  bool Native = false;
};

FrameRegion regionOf(CallInst *Enter, const AllocaInst *Frame) {
  FrameRegion Region;
  SmallPtrSet<BasicBlock *, 16> Visited;
  SmallVector<std::pair<BasicBlock *, BasicBlock::iterator>> Worklist;
  Worklist.push_back({Enter->getParent(), std::next(Enter->getIterator())});
  while (!Worklist.empty()) {
    auto [BB, It] = Worklist.pop_back_val();
    bool Stopped = false;
    for (; It != BB->end(); ++It) {
      Instruction &I = *It;
      if (auto *Call = dyn_cast<CallInst>(&I);
          Call && (calleeName(I) == LeaveFrameName || calleeName(I) == EnterFrameName) &&
          frameOf(*Call) == Frame) {
        Stopped = true;
        break;
      }
      if (calleeName(I) == SwitchToNativeName)
        Region.Native = true;
      if (isGCPoint(I)) {
        Region.GCPoints.push_back(&I);
        if (isa<InvokeInst>(I))
          Region.HasInvoke = true;
      }
    }
    if (Stopped)
      continue;
    Instruction *Terminator = BB->getTerminator();
    if (isa<ReturnInst>(Terminator) || isa<ResumeInst>(Terminator))
      Region.Leaks = true;
    for (BasicBlock *Succ : successors(BB)) {
      if (Visited.insert(Succ).second)
        Worklist.push_back({Succ, Succ->begin()});
    }
  }
  return Region;
}

CallInst *cloneFrameCall(Function *Callee, CallInst *Enter, Instruction *Before) {
  SmallVector<Value *> Args(Enter->args());
  auto *Call = CallInst::Create(Callee, Args, "", Before->getIterator());
  Call->setAttributes(Enter->getAttributes());
  Call->setCallingConv(Enter->getCallingConv());
  return Call;
}

bool shrinkWrapFunction(Function &F, Function *EnterFn, Function *LeaveFn) {
  auto FDebug = [&](const Twine &Why) {
    if (auto Filter = sys::Process::GetEnv("KORE_FRAMES_DEBUG"); Filter && F.getName().contains(*Filter))
      errs() << "kore-frames: " << F.getName() << ": " << Why << '\n';
  };
  MapVector<const AllocaInst *, SmallVector<CallInst *>> Enters;
  DenseMap<const AllocaInst *, SmallVector<Instruction *>> Others;
  for (Instruction &I : instructions(F)) {
    auto *Call = dyn_cast<CallBase>(&I);
    if (!Call || !isFrameCall(I))
      continue;
    const AllocaInst *Frame = frameOf(*Call);
    if (!Frame) {
      if (auto Filter = sys::Process::GetEnv("KORE_FRAMES_DEBUG"); Filter && F.getName().contains(*Filter)) {
        errs() << "kore-frames: " << F.getName() << ": no frame: " << *Call << " | arg: " << *Call->getArgOperand(0);
        if (auto *Arg = dyn_cast<Instruction>(Call->getArgOperand(0)))
          if (Arg->getNumOperands() > 0) errs() << " | op: " << *Arg->getOperand(0);
        errs() << '\n';
      }
      return false; // Not a frame we understand: leave the function alone.
    }
    if (calleeName(I) == EnterFrameName && isa<CallInst>(I))
      Enters[Frame].push_back(cast<CallInst>(&I));
    else
      Others[Frame].push_back(&I);
  }
  if (Enters.empty())
    return false;
  DominatorTree DT(F);
  LoopInfo LI(DT);
  // Frames to link around each call.
  MapVector<Instruction *, SmallVector<CallInst *>> Wraps;
  SmallVector<std::pair<CallInst *, const AllocaInst *>> Removed;
  for (auto &[Frame, FrameEnters] : Enters) {
    if (FrameEnters.size() != 1) {
      FDebug("several enters");
      continue;
    }
    CallInst *Enter = FrameEnters.front();
    auto Debug = [&](const char *Why) {
      if (auto Filter = sys::Process::GetEnv("KORE_FRAMES_DEBUG"); Filter && F.getName().contains(*Filter))
        errs() << "kore-frames: " << F.getName() << ": " << Why << '\n';
    };
    bool Unsupported = false;
    for (Instruction *I : Others[Frame])
      Unsupported |= calleeName(*I) != LeaveFrameName;
    if (Unsupported) {
      Debug("other frame calls");
      continue;
    }
    FrameRegion Region = regionOf(Enter, Frame);
    if (Region.Leaks || Region.HasInvoke || Region.Native) {
      Debug(Region.Leaks ? "leaks" : Region.HasInvoke ? "invoke" : "native");
      continue;
    }
    if (!Region.GCPoints.empty()) {
      // Linking around calls in loops would link the frame once per iteration.
      unsigned Depth = LI.getLoopDepth(Enter->getParent());
      bool InLoop = false;
      for (Instruction *Point : Region.GCPoints)
        InLoop |= LI.getLoopDepth(Point->getParent()) > Depth;
      if (InLoop) {
        Debug("calls in loops");
        continue;
      }
      if (sys::Process::GetEnv("KORE_FRAMES_NOLAZY")) {
        Debug("lazy disabled");
        continue;
      }
      Debug("lazy");
      for (Instruction *Point : Region.GCPoints)
        Wraps[Point].push_back(Enter);
    }
    if (Region.GCPoints.empty())
      Debug("elided");
    Removed.push_back({Enter, Frame});
  }
  bool Changed = false;
  for (auto &[Point, FrameEnters] : Wraps) {
    for (CallInst *Enter : FrameEnters)
      cloneFrameCall(EnterFn, Enter, Point);
    // Leave in the reverse order of entering: last in, first out.
    Instruction *Next = Point->getNextNode();
    for (CallInst *Enter : reverse(FrameEnters))
      cloneFrameCall(LeaveFn, Enter, Next);
    Changed = true;
  }
  for (auto &[Enter, Frame] : Removed) {
    for (Instruction *I : Others[Frame])
      I->eraseFromParent();
    Enter->eraseFromParent();
    Changed = true;
  }
  return Changed;
}

} // namespace

PreservedAnalyses ShrinkWrapFramesPass::run(Function &F, FunctionAnalysisManager &) {
  Module &M = *F.getParent();
  Function *EnterFn = M.getFunction(EnterFrameName);
  Function *LeaveFn = M.getFunction(LeaveFrameName);
  if (!EnterFn || !LeaveFn)
    return PreservedAnalyses::all();
  bool Changed = shrinkWrapFunction(F, EnterFn, LeaveFn);
  // Inline this function's frame calls, so that callers inline plain code.
  SmallVector<CallInst *> Calls;
  for (Instruction &I : instructions(F)) {
    auto *Call = dyn_cast<CallInst>(&I);
    if (Call && (Call->getCalledFunction() == EnterFn || Call->getCalledFunction() == LeaveFn))
      Calls.push_back(Call);
  }
  for (CallInst *Call : Calls) {
    Function *Callee = Call->getCalledFunction();
    Call->removeFnAttr(Attribute::NoInline);
    if (!Callee->isDeclaration() && !Callee->isInterposable()) {
      InlineFunctionInfo IFI;
      InlineFunction(*Call, IFI);
    }
    Changed = true;
  }
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

bool llvm::kotlin::shrinkWrapFrames(Module &M, bool Cleanup) {
  Function *EnterFn = M.getFunction(EnterFrameName);
  Function *LeaveFn = M.getFunction(LeaveFrameName);
  if (!EnterFn || !LeaveFn)
    return false;
  for (Function &F : M) {
    if (!F.isDeclaration())
      shrinkWrapFunction(F, EnterFn, LeaveFn);
  }
  // Inline the frame calls.
  SmallPtrSet<Function *, 32> Touched;
  for (Function *Callee : {EnterFn, LeaveFn, M.getFunction(SwitchToNativeName),
                           M.getFunction(SwitchToRunnableName)}) {
    if (!Callee)
      continue;
    SmallVector<CallInst *> Calls;
    for (User *U : Callee->users()) {
      if (auto *Call = dyn_cast<CallInst>(U); Call && Call->getCalledFunction() == Callee)
        Calls.push_back(Call);
    }
    for (CallInst *Call : Calls) {
      Touched.insert(Call->getFunction());
      Call->removeFnAttr(Attribute::NoInline);
      if (!Callee->isDeclaration() && !Callee->isInterposable()) {
        InlineFunctionInfo IFI;
        InlineFunction(*Call, IFI);
      }
    }
  }
  if (Touched.empty())
    return false;
  if (!Cleanup)
    return true;
  // Clean up: frames that are not linked any more are plain stack memory.
  PassBuilder PB;
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  FunctionPassManager FPM;
  if (auto Err = PB.parsePassPipeline(FPM, "sroa,early-cse<memssa>,dse,simplifycfg")) {
    consumeError(std::move(Err));
    return true;
  }
  for (Function *F : Touched)
    FPM.run(*F, FAM);
  return true;
}
