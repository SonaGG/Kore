// Copyright 2010-2026 JetBrains s.r.o. and Kotlin Programming Language
// contributors. Use of this source code is governed by the Apache 2.0 license
// that can be found in the license/LICENSE.txt file.

#include "CAPIExtensions.h"

#include "KotlinPlugin.h"
#include "PassesProfileHandler.h"

#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Transforms/Utils/SplitModule.h"

using namespace llvm;
using namespace llvm::kotlin;

static TargetMachine *unwrap(LLVMTargetMachineRef P) {
  return reinterpret_cast<TargetMachine *>(P);
}

void LLVMKotlinInitializeTargets() {
#define INIT_LLVM_TARGET(TargetName)                                           \
  LLVMInitialize##TargetName##TargetInfo();                                    \
  LLVMInitialize##TargetName##Target();                                        \
  LLVMInitialize##TargetName##TargetMC();

  INIT_LLVM_TARGET(AArch64)
  INIT_LLVM_TARGET(ARM)
  INIT_LLVM_TARGET(X86)

#undef INIT_LLVM_TARGET
}

void LLVMKotlinSetNoTailCall(LLVMValueRef Call) {
  unwrap<CallInst>(Call)->setTailCallKind(CallInst::TCK_NoTail);
}

int LLVMKotlinInlineCall(LLVMValueRef Call) {
  InlineFunctionInfo IFI;
  return InlineFunction(*unwrap<CallBase>(Call), IFI).isSuccess();
}

namespace {

class KotlinRunPassesCommandLineHolder {
public:
  KotlinRunPassesCommandLineHolder() {
    auto &Opts = cl::getRegisteredOptions();
    PrintAfter = Opts["print-after"];
    IrDumpDirectory = Opts["ir-dump-directory"];
  }

  // NOTE: Avoid adding new CLI arguments overrides as much as possible, as this
  //       makes `LLVMKotlinRunPasses` thread unsafe.
  //       When adding new ones, update `LLVMKotlinRunPasses` documentation in
  //       the header.
  Error Parse(const char *SaveIRAfterPasses, const char *SaveIRDirectory) {
    unsigned OptPos = 0;
    if (SaveIRAfterPasses != nullptr) {
      if (auto Err = SetOption(OptPos++, PrintAfter, SaveIRAfterPasses)) {
        return Err;
      }
    }
    if (SaveIRDirectory != nullptr) {
      if (auto Err = SetOption(OptPos++, IrDumpDirectory, SaveIRDirectory)) {
        return Err;
      }
    }
    return Error::success();
  }

  ~KotlinRunPassesCommandLineHolder() {
    for (auto *Opt : ModifiedOptions) {
      Opt->setDefault();
    }
  }

private:
  Error SetOption(unsigned OptPos, cl::Option *Opt, StringRef Val) {
    ModifiedOptions.push_back(Opt);
    if (Opt->getMiscFlags() & cl::MiscFlags::CommaSeparated) {
      StringRef::size_type Pos = Val.find(',');
      while (Pos != StringRef::npos) {
        auto SingleVal = Val.substr(0, Pos);
        if (Opt->addOccurrence(OptPos, Opt->ArgStr, SingleVal)) {
          return createStringError(Twine("Failed to parse value of ") +
                                   Opt->ArgStr + " :" + SingleVal);
        }
        // Erase the portion before the comma, AND the comma.
        Val = Val.substr(Pos + 1);
        // Check for another comma.
        Pos = Val.find(',');
      }
    }
    if (Opt->addOccurrence(OptPos, Opt->ArgStr, Val)) {
      return createStringError(Twine("Failed to parse value of ") +
                               Opt->ArgStr + " :" + Val);
    }
    return Error::success();
  }

  cl::Option *PrintAfter;
  cl::Option *IrDumpDirectory;
  SmallVector<cl::Option *> ModifiedOptions;
};

} // namespace

LLVMErrorRef LLVMKotlinRunPasses(LLVMModuleRef M, const char *Passes,
                                 LLVMTargetMachineRef TM, int InlinerThreshold,
                                 LLVMKotlinPassesProfileRef *Profile,
                                 const char *SaveIRAfterPasses,
                                 const char *SaveIRDirectory) {
  // Implementation is taken from
  // https://github.com/Kotlin/llvm-project/blob/0fa53d5183ec3c0654631d719dd6dfa7a270ca98/llvm/lib/Passes/PassBuilderBindings.cpp#L47
  TargetMachine *Machine = unwrap(TM);
  Module *Mod = unwrap(M);

  KotlinRunPassesCommandLineHolder CommandLineHolder;
  if (auto Err = CommandLineHolder.Parse(SaveIRAfterPasses, SaveIRDirectory)) {
    return wrap(std::move(Err));
  }

  PipelineTuningOptions PTO;
  PTO.InlinerThreshold = InlinerThreshold;
  PTO.MaxDevirtIterations = 0;
  PassInstrumentationCallbacks PIC;
  PassBuilder PB(Machine, PTO, std::nullopt, &PIC);

  // Register all Kotlin passes.
  getKotlinPluginInfo().RegisterPassBuilderCallbacks(PB);

  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PB.registerLoopAnalyses(LAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerModuleAnalyses(MAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  StandardInstrumentations SI(Mod->getContext(), false, false);
  SI.registerCallbacks(PIC, &MAM);

  PassesProfileHandler PPH(Profile != nullptr);
  // Putting last to make this the last callback for before* events;
  // the handler will additionally make sure its after* events are handled
  // before anything else. This makes it so the profile tracks phases only,
  // ignoring other callbacks.
  PPH.registerCallbacks(PIC);

  ModulePassManager MPM;
  if (auto Err = PB.parsePassPipeline(MPM, Passes))
    return wrap(std::move(Err));
  MPM.run(*Mod, MAM);

  if (Profile != nullptr) {
    *Profile = wrap(new PassesProfile(PPH.serialize()));
  }

  return LLVMErrorSuccess;
}

int LLVMKotlinSplitModule(LLVMModuleRef M, unsigned Partitions,
                          const char *PathPrefix) {
  Module *Mod = unwrap(M);
  // SplitModule externalizes locals referenced across partitions under their
  // own names, which may clash with external definitions of other objects
  // (e.g. internalized copies of libstdc++ functions), so make them unique.
  for (GlobalValue &GV : Mod->global_values()) {
    if (GV.hasLocalLinkage() && GV.hasName())
      GV.setName(GV.getName() + ".kore");
  }
  int Written = 0;
  bool Failed = false;
  SplitModule(*Mod, Partitions, [&](std::unique_ptr<Module> Part) {
    if (Failed)
      return;
    std::error_code EC;
    raw_fd_ostream OS(std::string(PathPrefix) + std::to_string(Written) + ".bc",
                      EC, sys::fs::OF_None);
    if (EC) {
      Failed = true;
      return;
    }
    WriteBitcodeToFile(*Part, OS);
    ++Written;
  });
  return Failed ? -1 : Written;
}

LLVMModuleRef LLVMKotlinSplitRuntimeModule(LLVMModuleRef Runtime,
                                           unsigned HotInstructionLimit,
                                           unsigned CalleeInstructionLimit,
                                           unsigned CalleeDepth) {
  Module &M = *unwrap(Runtime);

  // The program and the native object compiled from `M` refer to each other's
  // symbols, so nothing they share may be local or discardable. Locals are
  // renamed so that they do not clash with other objects' external symbols.
  for (GlobalValue &GV : M.global_values()) {
    if (GV.getName().starts_with("llvm."))
      continue;
    if (GV.hasLocalLinkage()) {
      GV.setName(GV.hasName() ? GV.getName() + ".kore.rt" : "kore.rt");
      GV.setLinkage(GlobalValue::ExternalLinkage);
      GV.setVisibility(GlobalValue::HiddenVisibility);
    } else if (GV.hasLinkOnceODRLinkage()) {
      GV.setLinkage(GlobalValue::WeakODRLinkage);
    } else if (GV.hasLinkOnceLinkage()) {
      GV.setLinkage(GlobalValue::WeakAnyLinkage);
    }
  }

  // Interposable definitions may be replaced by the program's ones.
  // The safepoint functions are `noinline` so that the compiler finds and inlines
  // them itself, after removing redundant ones.
  auto IsSafepoint = [](const Function &F) {
    return F.getName() == "Kotlin_mm_safePointFunctionPrologue" ||
           F.getName() == "Kotlin_mm_safePointWhileLoopBody" ||
           F.getName() == "EnterFrame" || F.getName() == "LeaveFrame" ||
           F.getName() == "Kotlin_mm_switchThreadStateNative" ||
           F.getName() == "Kotlin_mm_switchThreadStateRunnable";
  };
  auto MayInline = [&](const Function &F) {
    return !F.isDeclaration() && !F.isInterposable() &&
           !F.getName().starts_with("llvm.") &&
           (!F.hasFnAttribute(Attribute::NoInline) || IsSafepoint(F));
  };
  // Small functions, and the not too large functions they call (such as the
  // allocator's fast paths), which inlining into the program can flatten.
  DenseSet<const Function *> Inlinable;
  SmallVector<std::pair<const Function *, unsigned>> Worklist;
  for (const Function &F : M) {
    if (MayInline(F) && (F.hasFnAttribute(Attribute::AlwaysInline) ||
                         F.getInstructionCount() <= HotInstructionLimit)) {
      Inlinable.insert(&F);
      Worklist.push_back({&F, 0});
    }
  }
  while (!Worklist.empty()) {
    auto [F, Depth] = Worklist.pop_back_val();
    if (Depth >= CalleeDepth)
      continue;
    for (const Instruction &I : instructions(*F)) {
      const auto *Call = dyn_cast<CallBase>(&I);
      const Function *Callee = Call ? Call->getCalledFunction() : nullptr;
      if (!Callee || !MayInline(*Callee) || Callee->hasFnAttribute(Attribute::Cold) ||
          Callee->getInstructionCount() > CalleeInstructionLimit)
        continue;
      if (Inlinable.insert(Callee).second)
        Worklist.push_back({Callee, Depth + 1});
    }
  }

  auto KeepBody = [&](const GlobalValue *GV) {
    if (auto *F = dyn_cast<Function>(GV))
      return Inlinable.contains(F);
    if (auto *V = dyn_cast<GlobalVariable>(GV))
      return !V->getName().starts_with("llvm.") && !V->isInterposable() &&
             V->isConstant() && !V->isThreadLocal();
    return false;
  };

  ValueToValueMapTy VMap;
  std::unique_ptr<Module> Hot = CloneModule(M, VMap, KeepBody);
  for (const char *Name : {"llvm.global_ctors", "llvm.global_dtors",
                           "llvm.used", "llvm.compiler.used"}) {
    if (GlobalVariable *GV = Hot->getNamedGlobal(Name))
      GV->eraseFromParent();
  }
  for (GlobalObject &GO : Hot->global_objects()) {
    GO.setComdat(nullptr);
    if (GO.isDeclaration())
      continue;
    // The safepoint functions are inlined by the compiler's own passes after
    // the LTO pipeline, which drops `available_externally` bodies: give the
    // program copies of its own (internalized with the rest of the program).
    if (GO.getName() == "Kotlin_mm_safePointFunctionPrologue" ||
        GO.getName() == "Kotlin_mm_safePointWhileLoopBody" ||
        GO.getName() == "EnterFrame" || GO.getName() == "LeaveFrame" ||
        GO.getName() == "Kotlin_mm_switchThreadStateNative" ||
        GO.getName() == "Kotlin_mm_switchThreadStateRunnable")
      GO.setLinkage(GlobalValue::LinkOnceODRLinkage);
    else
      GO.setLinkage(GlobalValue::AvailableExternallyLinkage);
  }
  return wrap(Hot.release());
}

void LLVMKotlinAppendToUsed(LLVMModuleRef M, LLVMValueRef *Values,
                            unsigned Count) {
  SmallVector<GlobalValue *> GlobalValues;
  for (unsigned I = 0; I < Count; ++I)
    GlobalValues.push_back(unwrap<GlobalValue>(Values[I]));
  appendToUsed(*unwrap(M), GlobalValues);
}
