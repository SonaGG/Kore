// Copyright 2010-2026 JetBrains s.r.o. and Kotlin Programming Language
// contributors. Use of this source code is governed by the Apache 2.0 license
// that can be found in the license/LICENSE.txt file.

#include "CAPIExtensions.h"

#include "KotlinPlugin.h"
#include "PassesProfileHandler.h"
#include "RemoveRedundantSafepoints.h"

#include "llvm/ADT/StringSet.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Transforms/Utils/SplitModule.h"
#include "llvm-c/TargetMachine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <vector>

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
                                 const char *SaveIRDirectory,
                                 const char *PGOInstrumentFile,
                                 const char *PGOUseFile) {
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
  // Kore: instrumentation-based PGO, applied by the module simplification
  // pipeline (in particular the LTO pre-link one).
  std::optional<PGOOptions> PGOOpt;
  if (PGOInstrumentFile != nullptr) {
    PGOOpt = PGOOptions(PGOInstrumentFile, "", "", "", PGOOptions::IRInstr);
  } else if (PGOUseFile != nullptr) {
    PGOOpt = PGOOptions(PGOUseFile, "", "", "", PGOOptions::IRUse);
  }
  PassBuilder PB(Machine, PTO, PGOOpt, &PIC);

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

int LLVMKotlinSplitAndRunPasses(LLVMModuleRef M, unsigned Partitions,
                                const char *Passes, const char *Triple,
                                const char *CPU, const char *Features,
                                LLVMCodeGenOptLevel Level, LLVMRelocMode Reloc,
                                LLVMCodeModel CodeModel, int InlinerThreshold,
                                const char *PathPrefix) {
  Module *Mod = unwrap(M);
  // As in `LLVMKotlinSplitModule`: externalized locals must stay unique.
  for (GlobalValue &GV : Mod->global_values()) {
    if (GV.hasLocalLinkage() && GV.hasName())
      GV.setName(GV.getName() + ".kore");
  }
  // Each partition is optimized in its own context (contexts are not thread
  // safe), so it is passed on as bitcode.
  std::vector<SmallString<0>> Parts;
  SplitModule(*Mod, Partitions, [&](std::unique_ptr<Module> Part) {
    SmallString<0> Buffer;
    raw_svector_ostream OS(Buffer);
    WriteBitcodeToFile(*Part, OS);
    Parts.push_back(std::move(Buffer));
  });

  std::atomic<bool> Failed = false;
  std::vector<std::thread> Threads;
  for (size_t I = 0; I < Parts.size(); ++I) {
    Threads.emplace_back([&, I] {
      LLVMContext Context;
      auto Parsed = parseBitcodeFile(
          MemoryBufferRef(StringRef(Parts[I].data(), Parts[I].size()),
                          "partition"),
          Context);
      if (!Parsed) {
        consumeError(Parsed.takeError());
        Failed = true;
        return;
      }
      std::unique_ptr<Module> Part = std::move(*Parsed);
      Parts[I].clear();
      LLVMTargetRef Target;
      if (LLVMGetTargetFromTriple(Triple, &Target, nullptr) != 0) {
        Failed = true;
        return;
      }
      LLVMTargetMachineRef TM = LLVMCreateTargetMachine(
          Target, Triple, CPU, Features, Level, Reloc, CodeModel);
      LLVMErrorRef Err =
          LLVMKotlinRunPasses(wrap(Part.get()), Passes, TM, InlinerThreshold,
                              nullptr, nullptr, nullptr, nullptr, nullptr);
      LLVMDisposeTargetMachine(TM);
      if (Err != nullptr) {
        LLVMConsumeError(Err);
        Failed = true;
        return;
      }
      std::error_code EC;
      raw_fd_ostream OS(std::string(PathPrefix) + std::to_string(I) + ".bc",
                        EC, sys::fs::OF_None);
      if (EC) {
        Failed = true;
        return;
      }
      WriteBitcodeToFile(*Part, OS);
    });
  }
  for (auto &Thread : Threads)
    Thread.join();
  return Failed ? -1 : static_cast<int>(Parts.size());
}

int LLVMKotlinSplitOptimizeWholeProgram(
    LLVMModuleRef M, unsigned Partitions, const char *PreLinkPasses,
    const char *LtoPasses, int InlineSafepoints, const char *Triple,
    const char *CPU, const char *Features, LLVMCodeGenOptLevel Level,
    LLVMRelocMode Reloc, LLVMCodeModel CodeModel, int InlinerThreshold,
    unsigned ImportInstructionLimit, const char *PGOUseFile,
    const char *PathPrefix) {
  Module *Mod = unwrap(M);
  // As in `LLVMKotlinSplitModule`: externalized locals must stay unique.
  for (GlobalValue &GV : Mod->global_values()) {
    if (GV.hasLocalLinkage() && GV.hasName())
      GV.setName(GV.getName() + ".kore");
  }
  // Which partition defines what: the assignment of SplitModule (it keeps
  // COMDATs and the locals that refer to each other together, and externalizes
  // in `Mod` the locals referred to across partitions).
  std::vector<StringSet<>> Defined;
  SplitModule(*Mod, Partitions, [&](std::unique_ptr<Module> Part) {
    StringSet<> Names;
    for (GlobalValue &GV : Part->global_values()) // Aliases too (e.g. `main`).
      if (!GV.isDeclaration())
        Names.insert(GV.getName());
    Defined.push_back(std::move(Names));
  });

  // What each partition gets a read-only (`available_externally`) copy of: the
  // functions it calls that the inliner may inline (small ones, or the ones that
  // must be inlined, and in turn their small callees), and the constants its
  // code reads (type infos, vtables, ...), so that inlining and constant folding
  // see the same code as in the whole program.
  auto MayImport = [&](const Function &F) {
    return !F.isDeclaration() && F.hasName() && !F.isInterposable() &&
           !F.getName().starts_with("llvm.") &&
           !F.hasFnAttribute(Attribute::NoInline) &&
           (F.hasFnAttribute(Attribute::AlwaysInline) ||
            F.getInstructionCount() <= ImportInstructionLimit);
  };
  auto MayImportVariable = [](const GlobalVariable &V) {
    return V.hasInitializer() && V.isConstant() && V.hasName() &&
           !V.isInterposable() && !V.isThreadLocal() &&
           !V.getName().starts_with("llvm.");
  };
  std::vector<StringSet<>> Imported(Defined.size());
  DenseSet<GlobalValue *> ReferencedByImports;
  auto AddReferences = [&](Value *Root) {
    SmallVector<Value *> Stack{Root};
    DenseSet<Value *> Seen;
    while (!Stack.empty()) {
      Value *V = Stack.pop_back_val();
      if (!Seen.insert(V).second)
        continue;
      if (auto *Ref = dyn_cast<GlobalValue>(V)) {
        ReferencedByImports.insert(Ref);
      } else if (auto *C = dyn_cast<Constant>(V)) {
        for (Value *Op : C->operands())
          Stack.push_back(Op);
      }
    }
  };
  for (size_t P = 0; P < Defined.size(); ++P) {
    SmallVector<Function *> Worklist;
    std::function<void(Value *)> Use = [&](Value *V) {
      if (auto *CE = dyn_cast<ConstantExpr>(V)) {
        for (Value *Op : CE->operands())
          Use(Op);
        return;
      }
      auto *GV = dyn_cast<GlobalValue>(V);
      if (GV == nullptr || !GV->hasName() || Defined[P].contains(GV->getName()) ||
          Imported[P].contains(GV->getName()))
        return;
      if (auto *F = dyn_cast<Function>(GV)) {
        if (MayImport(*F)) {
          Imported[P].insert(F->getName());
          Worklist.push_back(F);
        }
      } else if (auto *Var = dyn_cast<GlobalVariable>(GV)) {
        if (MayImportVariable(*Var))
          Imported[P].insert(Var->getName());
      }
    };
    auto Scan = [&](Function &F) {
      for (Instruction &I : instructions(F))
        for (Value *Operand : I.operands())
          Use(Operand);
    };
    for (Function &F : *Mod)
      if (!F.isDeclaration() && F.hasName() && Defined[P].contains(F.getName()))
        Scan(F);
    while (!Worklist.empty())
      Scan(*Worklist.pop_back_val());
    for (auto &Entry : Imported[P]) {
      GlobalValue *GV = Mod->getNamedValue(Entry.getKey());
      if (GV == nullptr)
        continue;
      ReferencedByImports.insert(GV);
      if (auto *F = dyn_cast<Function>(GV)) {
        for (Instruction &I : instructions(*F))
          for (Value *Operand : I.operands())
            AddReferences(Operand);
      } else if (auto *Var = dyn_cast<GlobalVariable>(GV)) {
        AddReferences(Var->getInitializer());
      }
    }
  }
  // A copy refers to the symbols of its original, so the locals among those
  // become hidden externals (still defined once, in their own partition).
  for (GlobalValue *GV : ReferencedByImports) {
    if (GV->hasLocalLinkage()) {
      GV->setLinkage(GlobalValue::ExternalLinkage);
      GV->setVisibility(GlobalValue::HiddenVisibility);
    }
  }

  // The partitions, each with its definitions and copies, as bitcode: each is
  // optimized in its own context (contexts are not thread safe).
  std::vector<SmallString<0>> Parts;
  for (size_t P = 0; P < Defined.size(); ++P) {
    ValueToValueMapTy VMap;
    std::unique_ptr<Module> Part = CloneModule(*Mod, VMap, [&](const GlobalValue *GV) {
      return GV->hasName() && (Defined[P].contains(GV->getName()) || Imported[P].contains(GV->getName()));
    });
    for (GlobalObject &GO : Part->global_objects()) {
      if (GO.isDeclaration() || !Imported[P].contains(GO.getName()))
        continue;
      GO.setComdat(nullptr);
      GO.setLinkage(GlobalValue::AvailableExternallyLinkage);
      GO.setVisibility(GlobalValue::DefaultVisibility);
    }
    // Declarations must not keep local linkage (their definitions are elsewhere).
    for (GlobalValue &GV : Part->global_values()) {
      if (GV.isDeclaration() && GV.hasLocalLinkage()) {
        GV.setLinkage(GlobalValue::ExternalLinkage);
        GV.setVisibility(GlobalValue::HiddenVisibility);
      }
    }
    SmallString<0> Buffer;
    raw_svector_ostream OS(Buffer);
    WriteBitcodeToFile(*Part, OS);
    Parts.push_back(std::move(Buffer));
  }

  std::atomic<bool> Failed = false;
  std::vector<std::thread> Threads;
  for (size_t I = 0; I < Parts.size(); ++I) {
    Threads.emplace_back([&, I] {
      LLVMContext Context;
      auto Parsed = parseBitcodeFile(
          MemoryBufferRef(StringRef(Parts[I].data(), Parts[I].size()), "partition"), Context);
      if (!Parsed) {
        consumeError(Parsed.takeError());
        Failed = true;
        return;
      }
      std::unique_ptr<Module> Part = std::move(*Parsed);
      Parts[I].clear();
      LLVMTargetRef Target;
      if (LLVMGetTargetFromTriple(Triple, &Target, nullptr) != 0) {
        Failed = true;
        return;
      }
      LLVMTargetMachineRef TM = LLVMCreateTargetMachine(Target, Triple, CPU, Features, Level, Reloc, CodeModel);
      // The module pipeline (in its LTO pre-link form, which drops the copies at
      // its end), then the safepoint cleanup, then the LTO pipeline: as on the
      // whole program.
      LLVMErrorRef Err = LLVMKotlinRunPasses(wrap(Part.get()), PreLinkPasses, TM, InlinerThreshold,
                                             nullptr, nullptr, nullptr, nullptr, PGOUseFile);
      if (Err == nullptr) {
        LLVMKotlinRemoveRedundantSafepoints(wrap(Part.get()), InlineSafepoints);
        Err = LLVMKotlinRunPasses(wrap(Part.get()), LtoPasses, TM, InlinerThreshold,
                                  nullptr, nullptr, nullptr, nullptr, nullptr);
      }
      LLVMDisposeTargetMachine(TM);
      if (Err != nullptr) {
        LLVMConsumeError(Err);
        Failed = true;
        return;
      }
      std::error_code EC;
      raw_fd_ostream OS(std::string(PathPrefix) + std::to_string(I) + ".bc", EC, sys::fs::OF_None);
      if (EC) {
        Failed = true;
        return;
      }
      WriteBitcodeToFile(*Part, OS);
    });
  }
  for (auto &Thread : Threads)
    Thread.join();
  return Failed ? -1 : static_cast<int>(Parts.size());
}

namespace {

constexpr const char *kKorePartitionAttribute = "kore-partition";

unsigned partitionOf(const GlobalObject &GO) {
  if (auto *F = dyn_cast<Function>(&GO)) {
    Attribute A = F->getFnAttribute(kKorePartitionAttribute);
    if (A.isStringAttribute()) {
      unsigned P = 0;
      if (!A.getValueAsString().getAsInteger(10, P))
        return P;
    }
  } else if (auto *V = dyn_cast<GlobalVariable>(&GO)) {
    if (V->hasAttribute(kKorePartitionAttribute)) {
      unsigned P = 0;
      if (!V->getAttribute(kKorePartitionAttribute).getValueAsString().getAsInteger(10, P))
        return P;
    }
  }
  return 0;
}

} // namespace

int LLVMKotlinAssignPartitions(LLVMModuleRef M, unsigned Partitions) {
  Module *Mod = unwrap(M);
  if (Partitions <= 1)
    return 1;
  // Locals become hidden externals (renamed to stay unique): any partition may
  // refer to them. Without optimization, this does not change the code.
  for (GlobalValue &GV : Mod->global_values()) {
    if (GV.getName().starts_with("llvm."))
      continue;
    if (GV.hasLocalLinkage()) {
      if (GV.hasName())
        GV.setName(GV.getName() + ".kore");
      else
        GV.setName("kore.unnamed");
      GV.setLinkage(GlobalValue::ExternalLinkage);
      GV.setVisibility(GlobalValue::HiddenVisibility);
    }
  }
  // Groups that must stay together: the members of a COMDAT, and aliases with
  // what they alias. Each group goes to the partition with the least code so far.
  DenseMap<const Comdat *, unsigned> ComdatPartition;
  std::vector<uint64_t> Size(Partitions, 0);
  auto Lightest = [&] {
    return static_cast<unsigned>(std::min_element(Size.begin(), Size.end()) - Size.begin());
  };
  auto Assign = [&](GlobalObject &GO, unsigned P) {
    std::string Value = std::to_string(P);
    if (auto *F = dyn_cast<Function>(&GO)) {
      F->addFnAttr(kKorePartitionAttribute, Value);
      Size[P] += F->getInstructionCount() + 1;
    } else if (auto *V = dyn_cast<GlobalVariable>(&GO)) {
      V->addAttribute(kKorePartitionAttribute, Value);
      Size[P] += 1;
    }
  };
  for (GlobalObject &GO : Mod->global_objects()) {
    if (GO.isDeclaration() || GO.getName().starts_with("llvm."))
      continue;
    unsigned P;
    if (const Comdat *C = GO.getComdat()) {
      auto It = ComdatPartition.find(C);
      P = It != ComdatPartition.end() ? It->second : (ComdatPartition[C] = Lightest());
    } else {
      P = Lightest();
    }
    Assign(GO, P);
  }
  return static_cast<int>(Partitions);
}

int LLVMKotlinCodegenPartitionsInParallel(const char *BitcodePath,
                                          unsigned Partitions,
                                          const char *PathPrefix) {
  // Each thread reads the whole program into its own context and keeps the
  // definitions of its partition (see `LLVMKotlinAssignPartitions`): reading in
  // parallel is cheaper than splitting one module into copies, one at a time.
  auto Buffer = MemoryBuffer::getFile(BitcodePath);
  if (!Buffer)
    return -1;
  std::atomic<bool> Failed = false;
  std::vector<std::thread> Threads;
  for (unsigned I = 0; I < Partitions; ++I) {
    Threads.emplace_back([&, I] {
      LLVMContext Context;
      // Lazily: only the bodies of this partition's functions are read (their
      // attributes, which say where they belong, are read with the module).
      auto Parsed = getLazyBitcodeModule((*Buffer)->getMemBufferRef(), Context,
                                         /* ShouldLazyLoadMetadata = */ true);
      if (!Parsed) {
        consumeError(Parsed.takeError());
        Failed = true;
        return;
      }
      std::unique_ptr<Module> Part = std::move(*Parsed);
      // Aliases of other partitions become declarations of what they alias (first:
      // an alias must not point to a declaration when the module is verified).
      SmallVector<GlobalAlias *> ForeignAliases;
      for (GlobalAlias &GA : Part->aliases()) {
        auto *Target = dyn_cast<GlobalObject>(GA.getAliaseeObject());
        if (Target != nullptr && !Target->isDeclaration() && partitionOf(*Target) != I)
          ForeignAliases.push_back(&GA);
      }
      for (GlobalAlias *GA : ForeignAliases) {
        GlobalValue *Declaration;
        if (auto *F = dyn_cast<Function>(GA->getAliaseeObject())) {
          Declaration = Function::Create(F->getFunctionType(), GlobalValue::ExternalLinkage, "", Part.get());
        } else {
          Declaration = new GlobalVariable(*Part, GA->getValueType(), false, GlobalValue::ExternalLinkage, nullptr);
        }
        Declaration->takeName(GA);
        GA->replaceAllUsesWith(Declaration);
        GA->eraseFromParent();
      }
      for (Function &F : *Part) {
        if (!F.isMaterializable())
          continue;
        if (partitionOf(F) == I) {
          if (Error E = F.materialize()) {
            consumeError(std::move(E));
            Failed = true;
            return;
          }
        } else {
          F.deleteBody();
          F.setComdat(nullptr);
          F.setLinkage(GlobalValue::ExternalLinkage);
        }
      }
      if (Error E = Part->materializeAll()) {
        consumeError(std::move(E));
        Failed = true;
        return;
      }
      // Lazily read metadata may attach the debug info of a definition to what is
      // now a declaration.
      for (Function &F : *Part)
        if (F.isDeclaration())
          F.clearMetadata();
      for (GlobalObject &GO : Part->global_objects()) {
        if (GO.isDeclaration() || GO.getName().starts_with("llvm.") || partitionOf(GO) == I)
          continue;
        if (auto *F = dyn_cast<Function>(&GO)) {
          F->deleteBody();
        } else if (auto *V = dyn_cast<GlobalVariable>(&GO)) {
          V->setInitializer(nullptr);
          V->eraseMetadata(LLVMContext::MD_dbg);
        }
        GO.setComdat(nullptr);
        GO.setLinkage(GlobalValue::ExternalLinkage);
      }
      // Module-level inline assembly and static constructors belong to one part.
      if (I != 0) {
        Part->setModuleInlineAsm("");
        for (const char *Name : {"llvm.global_ctors", "llvm.global_dtors", "llvm.used", "llvm.compiler.used"})
          if (GlobalVariable *GV = Part->getNamedGlobal(Name))
            GV->eraseFromParent();
      }
      std::error_code EC;
      raw_fd_ostream OS(std::string(PathPrefix) + std::to_string(I) + ".bc", EC, sys::fs::OF_None);
      if (EC) {
        Failed = true;
        return;
      }
      WriteBitcodeToFile(*Part, OS);
    });
  }
  for (auto &Thread : Threads)
    Thread.join();
  return Failed ? -1 : static_cast<int>(Partitions);
}
