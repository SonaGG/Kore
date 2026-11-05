// Copyright 2010-2026 JetBrains s.r.o. and Kotlin Programming Language
// contributors. Use of this source code is governed by the Apache 2.0 license
// that can be found in the license/LICENSE.txt file.

#include "KotlinPlugin.h"

#include "Passes/HideSymbols.h"
#include "Passes/Frames.h"
#include "Passes/LoopCallBonus.h"
#include "Passes/LoopSafepoints.h"
#include "Passes/ObjectScalarReplacement.h"
#include "Passes/PrepareStackProtector.h"
#include "Passes/PrepareThreadSanitizer.h"

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/Scalar/InductiveRangeCheckElimination.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Process.h"

#include <optional>

using namespace llvm;
using namespace llvm::kotlin;

static Expected<SspMode> ParseSspModePassOptions(StringRef Params) {
  if (Params.empty()) {
    return SspMode::Default;
  }
  if (Params == "strong") {
    return SspMode::Strong;
  }
  if (Params == "req") {
    return SspMode::Req;
  }
  return make_error<StringError>(
      formatv("invalid kotlin-ssp pass parameter '{0}'", Params).str(),
      inconvertibleErrorCode());
}

static bool parsePass(StringRef Name, StringRef PassName) {
  return Name == PassName;
}

template <typename F>
static auto parsePass(StringRef Name, StringRef PassName, F &&Parser)
    -> std::optional<typename decltype(Parser(StringRef{}))::value_type> {
  if (!PassBuilder::checkParametrizedPassName(Name, PassName)) {
    return std::nullopt;
  }
  auto Param =
      PassBuilder::parsePassParameters(std::forward<F>(Parser), Name, PassName);
  if (auto E = Param.takeError()) {
    reportFatalUsageError(std::move(E));
  }
  return std::make_optional(std::move(*Param));
}

// Kore: GC frame shrink-wrapping (Passes/Frames.h). Off: it slows down some
// loops (frame calls kept opaque during inlining) more than it gains.
static constexpr bool ShrinkWrapFrames = false;

// Kore: for bisecting miscompilations, `KORE_DISABLE_PASSES` lists Kore passes
// to leave out (e.g. `osr,irce`).
static bool passDisabled(StringRef Name) {
  auto Disabled = sys::Process::GetEnv("KORE_DISABLE_PASSES");
  if (!Disabled)
    return false;
  SmallVector<StringRef> Names;
  StringRef(*Disabled).split(Names, ',');
  return is_contained(Names, Name);
}

// Kore: optimizations LLVM has but does not enable by default (some of which
// other compilers enable at their highest levels): constraint-based removal
// of redundant (bounds) checks, jump threading of state machines, loop
// interchange, flattening and unroll-and-jam, loop versioning for invariant
// code motion, hoisting and sinking of common code, extra cleanups after
// vectorization, and interprocedural register allocation.
static void enableExtraOptimizations() {
  static bool Done = false;
  if (Done)
    return;
  Done = true;
  if (sys::Process::GetEnv("KORE_NO_EXTRA_OPTS"))
    return;
  auto &Options = cl::getRegisteredOptions();
  for (const char *Name :
       {"enable-constraint-elimination", "enable-dfa-jump-thread",
        "enable-loopinterchange", "enable-loop-flatten", "enable-unroll-and-jam",
        "enable-loop-versioning-licm", "enable-gvn-hoist", "enable-gvn-sink",
        "extra-vectorizer-passes", "enable-ipra",
        "enable-interleaved-mem-accesses", "enable-partial-inlining",
        "hot-cold-split"}) {
    auto It = Options.find(Name);
    if (It != Options.end())
      It->second->addOccurrence(0, Name, "true");
  }
  // For experiments: `KORE_LLVM_OPTS="name=value name=value"` sets LLVM
  // options.
  if (std::optional<std::string> Extra = sys::Process::GetEnv("KORE_LLVM_OPTS")) {
    SmallVector<StringRef> Settings;
    StringRef(*Extra).split(Settings, ' ', -1, false);
    for (StringRef Setting : Settings) {
      auto [Name, Value] = Setting.split('=');
      auto It = Options.find(Name);
      if (It != Options.end())
        It->second->addOccurrence(0, Name, Value.empty() ? "true" : Value);
    }
  }
}

PassPluginLibraryInfo getKotlinPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "Kotlin", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            enableExtraOptimizations();
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &PM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (parsePass(Name, "kotlin-hide-symbols")) {
                    PM.addPass(HideSymbolsPass());
                    return true;
                  }
                  return false;
                });
            // Kore: decide on loop safepoints once loops are simplified, but
            // before they are vectorized.
            PB.registerScalarOptimizerLateEPCallback(
                [](FunctionPassManager &PM, OptimizationLevel) {
                  // Replace objects by their fields while allocations are
                  // still calls.
                  if (!passDisabled("osr"))
                    PM.addPass(ObjectScalarReplacementPass());
                  PM.addPass(LoopSafepointsPass());
                  // Then place GC frames, now that safepoints are decided.
                  if (ShrinkWrapFrames)
                    PM.addPass(ShrinkWrapFramesPass());
                  // Then split loops into one without array bounds checks and
                  // (rarely run) ones with them, so that they can be vectorized.
                  if (!passDisabled("irce"))
                    PM.addPass(IRCEPass());
                });
            // Kore: static hotness for the inliner.
            PB.registerPipelineEarlySimplificationEPCallback(
                [](ModulePassManager &PM, OptimizationLevel, ThinOrFullLTOPhase) {
                  if (!passDisabled("callbonus"))
                    PM.addPass(createModuleToFunctionPassAdaptor(LoopCallBonusPass()));
                  if (ShrinkWrapFrames)
                    PM.addPass(PendingFramesPass());
                });
            // Kore: allocations stay calls until all inlining into their
            // functions is done.
            PB.registerOptimizerEarlyEPCallback(
                [](ModulePassManager &PM, OptimizationLevel, ThinOrFullLTOPhase) {
                  PM.addPass(InlinePendingAllocationsPass());
                });
            PB.registerFullLinkTimeOptimizationEarlyEPCallback(
                [](ModulePassManager &PM, OptimizationLevel) {
                  PM.addPass(InlinePendingAllocationsPass());
                });
            // The LTO pipeline has no late scalar optimizer extension point, but
            // it inlines too: remove the prologue polls of callees it inlines.
            PB.registerPeepholeEPCallback(
                [](FunctionPassManager &PM, OptimizationLevel) {
                  PM.addPass(LoopSafepointsPass(/*PrologueOnly=*/true));
                });
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &PM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (parsePass(Name, "kotlin-loop-safepoints")) {
                    PM.addPass(LoopSafepointsPass());
                    return true;
                  }
                  if (parsePass(Name, "kotlin-tsan")) {
                    PM.addPass(PrepareThreadSanitizerPass());
                    return true;
                  }
                  if (auto Param = parsePass(Name, "kotlin-ssp",
                                             ParseSspModePassOptions)) {
                    PM.addPass(PrepareStackProtectorPass(*Param));
                    return true;
                  }
                  return false;
                });
          }};
}

// This macro is unlikely to be useful in the foreseeable future.
// Just following llvm/examples/Bye verbatim.
#ifndef LLVM_KOTLIN_LINK_INTO_TOOLS
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return getKotlinPluginInfo();
}
#endif
