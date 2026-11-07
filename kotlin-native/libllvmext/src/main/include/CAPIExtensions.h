// Copyright 2010-2026 JetBrains s.r.o. and Kotlin Programming Language
// contributors. Use of this source code is governed by the Apache 2.0 license
// that can be found in the license/LICENSE.txt file.

#ifndef LIBLLVMEXT_C_API_EXTENSIONS_H
#define LIBLLVMEXT_C_API_EXTENSIONS_H

#include "PassesProfile.h"

#include "llvm-c/Error.h"
#include "llvm-c/ExternC.h"
#include "llvm-c/TargetMachine.h"
#include "llvm-c/Types.h"

LLVM_C_EXTERN_C_BEGIN

void LLVMKotlinInitializeTargets(void);

void LLVMKotlinSetNoTailCall(LLVMValueRef Call);

int LLVMKotlinInlineCall(LLVMValueRef Call);

/// Run `Passes` on module `M`.
/// When `Profile` is not `NULL` also collect profiling data and store the
/// result in it.
///
/// NOTE: This function is not thread-safe, because it may write
///       into global variables by modifying CLI-defined options.
///       Currently these arguments affect the global environment:
///       - SaveIRAfterPasses
///       - SaveIRDirectory
///
/// Kore: when `PGOInstrumentFile` is not `NULL`, `M` is instrumented for
/// profile-guided optimization, and the instrumented program writes its raw
/// profile to `PGOInstrumentFile` (a pattern, as in `LLVM_PROFILE_FILE`). When
/// `PGOUseFile` is not `NULL`, `M` is optimized using the indexed profile
/// `PGOUseFile` (produced by `llvm-profdata merge`). Either only takes effect in
/// pipelines that simplify the module, such as `default<O3>` and
/// `lto-pre-link<O3>`.
LLVMErrorRef LLVMKotlinRunPasses(LLVMModuleRef M, const char *Passes,
                                 LLVMTargetMachineRef TM, int InlinerThreshold,
                                 LLVMKotlinPassesProfileRef *Profile,
                                 const char *SaveIRAfterPasses,
                                 const char *SaveIRDirectory,
                                 const char *PGOInstrumentFile,
                                 const char *PGOUseFile);

/// Kore: splits `M` into `Partitions` modules for parallel code generation and
/// writes partition `i` as bitcode to `<PathPrefix><i>.bc`. Local symbols
/// referenced across partitions are externalized with hidden visibility, so `M`
/// is modified. Returns the number of files written, or -1 on an I/O error.
int LLVMKotlinSplitModule(LLVMModuleRef M, unsigned Partitions,
                          const char *PathPrefix);

/// Kore: like `LLVMKotlinSplitModule`, but each partition is then optimized with
/// the pipeline `Passes` (e.g. `lto<O3>`), in parallel: in its own context, with
/// its own target machine. Writes partition `i` to `<PathPrefix><i>.bc`. `M` is
/// modified. Returns the number of files written, or -1 on an error.
int LLVMKotlinSplitAndRunPasses(LLVMModuleRef M, unsigned Partitions,
                                const char *Passes, const char *Triple,
                                const char *CPU, const char *Features,
                                LLVMCodeGenOptLevel Level, LLVMRelocMode Reloc,
                                LLVMCodeModel CodeModel, int InlinerThreshold,
                                const char *PathPrefix);

/// Kore: parallel code generation without optimization. Assigns every
/// definition of `M` to one of `Partitions` partitions (balanced by size,
/// COMDAT groups together) with the `kore-partition` attribute, and makes locals
/// hidden externals (renamed to stay unique). Returns the number of partitions.
int LLVMKotlinAssignPartitions(LLVMModuleRef M, unsigned Partitions);

/// Kore: reads the bitcode of a module prepared by `LLVMKotlinAssignPartitions`
/// on `Partitions` threads, each keeping the definitions of its partition, and
/// writes partition `i` to `<PathPrefix><i>.bc`. Returns the number of files
/// written, or -1 on an error.
int LLVMKotlinCodegenPartitionsInParallel(const char *BitcodePath,
                                          unsigned Partitions,
                                          const char *PathPrefix);

/// Kore: the whole optimization of a closed-world program, in parallel. `M` (not
/// optimized yet) is split into `Partitions` partitions, each with read-only
/// (`available_externally`) copies of the functions of the others that it calls
/// and the inliner may inline (at most `ImportInstructionLimit` instructions, or
/// always inlined) and of the constants it reads. Each partition then runs the
/// module pipeline `PreLinkPasses` (with the profile `PGOUseFile`, if any), the
/// safepoint cleanup, and the LTO pipeline `LtoPasses`, on its own thread.
/// Writes partition `i` to `<PathPrefix><i>.bc`. `M` is modified. Returns the
/// number of files written (one, if `M` is too small to split), or -1 on an error.
int LLVMKotlinSplitOptimizeWholeProgram(
    LLVMModuleRef M, unsigned Partitions, const char *PreLinkPasses,
    const char *LtoPasses, int InlineSafepoints, const char *Triple,
    const char *CPU, const char *Features, LLVMCodeGenOptLevel Level,
    LLVMRelocMode Reloc, LLVMCodeModel CodeModel, int InlinerThreshold,
    unsigned ImportInstructionLimit, const char *PGOUseFile,
    const char *PathPrefix);

/// Kore: prepares the linked runtime module `Runtime` to be compiled once into a
/// native object that is shared by many programs, and returns a new module in
/// the same context to link into a program instead of the runtime.
///
/// `Runtime` is modified: local symbols are externalized (renamed to stay
/// unique, with hidden visibility) and discardable definitions are made
/// non-discardable, so that the object compiled from it defines everything the
/// returned module refers to.
///
/// The returned module declares everything defined by `Runtime`, except that
/// it keeps the bodies of small functions (at most `HotInstructionLimit`
/// instructions), of `alwaysinline` functions, and of the functions they call
/// (transitively, up to `CalleeDepth` calls deep) that have at most
/// `CalleeInstructionLimit` instructions, and the initializers of constants, as
/// `available_externally` definitions, so that they can still be inlined into
/// the program.
LLVMModuleRef LLVMKotlinSplitRuntimeModule(LLVMModuleRef Runtime,
                                           unsigned HotInstructionLimit,
                                           unsigned CalleeInstructionLimit,
                                           unsigned CalleeDepth);

/// Kore: appends `Values` to `llvm.used` of `M`.
void LLVMKotlinAppendToUsed(LLVMModuleRef M, LLVMValueRef *Values,
                            unsigned Count);

LLVM_C_EXTERN_C_END

#endif
