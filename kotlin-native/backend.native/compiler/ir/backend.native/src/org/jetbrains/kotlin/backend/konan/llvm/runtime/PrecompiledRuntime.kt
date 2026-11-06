/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

package org.jetbrains.kotlin.backend.konan.llvm.runtime

import kotlinx.cinterop.allocArrayOf
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.readBytes
import llvm.*
import org.jetbrains.kotlin.backend.konan.BitcodeCompiler
import org.jetbrains.kotlin.backend.konan.LTOOptimizationPipeline
import org.jetbrains.kotlin.backend.konan.LlvmPipelineConfig
import org.jetbrains.kotlin.backend.konan.MandatoryOptimizationPipeline
import org.jetbrains.kotlin.backend.konan.ModuleOptimizationPipeline
import org.jetbrains.kotlin.backend.konan.NativeGenerationState
import org.jetbrains.kotlin.backend.konan.createLTOPipelineConfigForRuntime
import org.jetbrains.kotlin.backend.konan.llvm.getName
import org.jetbrains.kotlin.backend.konan.llvm.llvmLinkModules2
import org.jetbrains.kotlin.backend.konan.llvm.name
import org.jetbrains.kotlin.config.nativeBinaryOptions.RuntimeLinkageStrategy
import org.jetbrains.kotlin.konan.config.overrideClangOptions
import org.jetbrains.kotlin.konan.target.ClangFlags
import org.jetbrains.kotlin.konan.target.CompilerOutputKind
import java.io.File
import java.nio.channels.FileChannel
import java.nio.channels.FileLock
import java.nio.channels.OverlappingFileLockException
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.nio.file.StandardOpenOption
import java.security.MessageDigest
import java.util.concurrent.ConcurrentHashMap

/**
 * Kore: the runtime of an optimized program, split by [linkPrecompiledRuntime].
 *
 * @property inlinableModule declares the runtime and keeps the bodies of functions worth inlining into the program.
 * @property importedNames the symbols the runtime refers to but does not define, or defines overridably, some of which
 * are defined by the program.
 */
internal class PrecompiledRuntime(val inlinableModule: LLVMModuleRef, private val importedNames: List<String>) {
    /**
     * The program's definitions of the runtime's imports are referenced from the separately compiled runtime object, so
     * they must stay external and survive dead code elimination of the program module (`llvm.compiler.used` would not keep
     * them external).
     */
    fun preserveImports(programModule: LLVMModuleRef) {
        val definitions = importedNames.mapNotNull { name ->
            (LLVMGetNamedFunction(programModule, name) ?: LLVMGetNamedGlobal(programModule, name))
                    ?.takeIf { LLVMIsDeclaration(it) == 0 }
        }
        if (definitions.isEmpty()) return
        memScoped {
            LLVMKotlinAppendToUsed(programModule, allocArrayOf(definitions), definitions.size)
        }
    }
}

/**
 * Kore: the LTO pipelines of an optimized program spend most of their time re-optimizing the C++ runtime, which is the
 * same for every program. Instead, compile the runtime once into a native object (cached in the Konan data directory,
 * keyed by the runtime bitcode and compilation flags) and link the program against it, giving LTO only the bodies of
 * small runtime functions to inline.
 *
 * Returns `null` if the runtime has to be linked into the program module as usual.
 */
internal fun linkPrecompiledRuntime(generationState: NativeGenerationState, runtimeNativeLibraries: List<LLVMModuleRef>): PrecompiledRuntime? {
    if (runtimeNativeLibraries.isEmpty() || !shouldPrecompileRuntime(generationState)) return null

    runtimeNativeLibraries.forEach { prepareRuntimeModule(generationState, it) }
    val runtimeModule = LLVMModuleCreateWithNameInContext("runtime", generationState.llvmContext)!!
    try {
        runtimeNativeLibraries.forEach {
            val failed = llvmLinkModules2(generationState, runtimeModule, it)
            if (failed != 0) {
                throw Error("Failed to link ${it.getName()}")
            }
        }
        val importedNames = generateSequence(LLVMGetFirstFunction(runtimeModule), ::LLVMGetNextFunction)
                .plus(generateSequence(LLVMGetFirstGlobal(runtimeModule), ::LLVMGetNextGlobal))
                // Also the runtime's overridable definitions: the program may replace them, e.g. with binary option values.
                .filter { LLVMIsDeclaration(it) != 0 || LLVMGetLinkage(it) in overridableLinkages }
                .map { it.name.orEmpty() }
                .filterNot { it.isEmpty() || it.startsWith("llvm.") }
                .toList()
        val inlinableModule = LLVMKotlinSplitRuntimeModule(runtimeModule, HOT_INSTRUCTION_LIMIT, CALLEE_INSTRUCTION_LIMIT, CALLEE_DEPTH)!!
        generationState.precompiledRuntimeObjects += precompiledRuntimeObject(generationState, runtimeModule)
        return PrecompiledRuntime(inlinableModule, importedNames)
    } finally {
        LLVMDisposeModule(runtimeModule)
    }
}

private fun shouldPrecompileRuntime(generationState: NativeGenerationState): Boolean {
    val config = generationState.config
    return config.precompileRuntime
            // Only executables: precompiled runtime symbols are external, so MinGW would export them from a DLL.
            && config.produce == CompilerOutputKind.PROGRAM
            && config.optimizationsEnabled
            && config.runtimeLinkageStrategy == RuntimeLinkageStrategy.Raw
            && !config.checkStateAtExternalCalls
            && config.sanitizer == null
            // With PGO, the runtime is optimized with the program, so that the profile covers its hot paths (e.g. allocation):
            // the bodies of the precompiled runtime kept for inlining get no profile, and profiled code then inlines them less.
            && !config.pgoGenerate && config.pgoUse == null
            // The runtime object is passed to the linker by the same compiler invocation that links the runtime.
            && config.writeSerializedDependencies.isNullOrEmpty()
}

private fun precompiledRuntimeObject(generationState: NativeGenerationState, runtimeModule: LLVMModuleRef): File {
    val config = generationState.config
    val pipelineConfig = createLTOPipelineConfigForRuntime(generationState)
    val digest = MessageDigest.getInstance("SHA-256")
    val bitcode = LLVMWriteBitcodeToMemoryBuffer(runtimeModule)!!
    try {
        digest.update(LLVMGetBufferStart(bitcode)!!.readBytes(LLVMGetBufferSize(bitcode).toInt()))
    } finally {
        LLVMDisposeMemoryBuffer(bitcode)
    }
    val clangFlags = (config.platform.configurables as? ClangFlags)?.let { it.clangFlags + it.clangOptFlags }.orEmpty()
    val key = listOf(
            CACHE_FORMAT,
            pipelineConfig.copy(timePasses = false, saveIrAfterPasses = emptyList(), saveIrDirectory = null).toString(),
            clangFlags.joinToString(" "),
            config.configuration.overrideClangOptions.joinToString(" "),
    )
    digest.update(key.joinToString("\n").toByteArray())
    val hash = digest.digest().joinToString("") { "%02x".format(it) }

    val cacheDirectory = config.distribution.localKonanDir.resolve("kore-cache/runtime/${config.target.name}")
    val objectFile = cacheDirectory.resolve("$hash.o")
    if (objectFile.isFile) return objectFile

    cacheDirectory.mkdirs()
    // Compilations in this and other processes (parallel Gradle tasks, daemons) wait for one of them to produce the
    // object rather than racing to replace a file another linker may have open.
    synchronized(cacheLocks.computeIfAbsent(objectFile.path) { Any() }) {
        FileChannel.open(cacheDirectory.resolve("$hash.lock").toPath(), StandardOpenOption.CREATE, StandardOpenOption.WRITE).use { channel ->
            channel.lockWaitingForOtherClassLoaders().use {
                if (!objectFile.isFile) {
                    compileRuntimeObject(generationState, runtimeModule, pipelineConfig, cacheDirectory, objectFile)
                }
            }
        }
    }
    return objectFile
}

private val cacheLocks = ConcurrentHashMap<String, Any>()

// Compilers loaded by other class loaders of this JVM (e.g. a Gradle daemon) do not share [cacheLocks], and the JVM refuses
// overlapping file locks of the same process instead of blocking.
private fun FileChannel.lockWaitingForOtherClassLoaders(): FileLock {
    while (true) {
        try {
            return lock()
        } catch (_: OverlappingFileLockException) {
            Thread.sleep(20)
        }
    }
}

private fun compileRuntimeObject(
        generationState: NativeGenerationState,
        runtimeModule: LLVMModuleRef,
        pipelineConfig: LlvmPipelineConfig,
        cacheDirectory: File,
        objectFile: File,
) {
    val bitcodeFile = File.createTempFile("runtime-", ".bc", cacheDirectory)
    val temporaryObjectFile = File.createTempFile("runtime-", ".o", cacheDirectory)
    try {
        MandatoryOptimizationPipeline(pipelineConfig, generationState.performanceManager, generationState).use {
            it.execute(runtimeModule)
        }
        // The object is cached, so optimize as thoroughly as a program's module and LTO pipelines would.
        ModuleOptimizationPipeline(pipelineConfig, generationState.performanceManager, generationState, preLink = true).use {
            it.execute(runtimeModule)
        }
        LTOOptimizationPipeline(pipelineConfig, generationState.performanceManager, generationState).use {
            it.execute(runtimeModule)
        }
        LLVMWriteBitcodeToFile(runtimeModule, bitcodeFile.path)
        BitcodeCompiler(generationState).makeObjectFile(bitcodeFile, temporaryObjectFile)
        // Atomically, so that compilations checking for the object without the lock never see a partial one.
        try {
            Files.move(temporaryObjectFile.toPath(), objectFile.toPath(), StandardCopyOption.ATOMIC_MOVE)
        } catch (_: AtomicMoveNotSupportedException) {
            Files.move(temporaryObjectFile.toPath(), objectFile.toPath())
        }
    } finally {
        bitcodeFile.delete()
        temporaryObjectFile.delete()
    }
}

private val overridableLinkages = setOf(
        LLVMLinkage.LLVMWeakAnyLinkage, LLVMLinkage.LLVMLinkOnceAnyLinkage, LLVMLinkage.LLVMCommonLinkage, LLVMLinkage.LLVMExternalWeakLinkage,
)

// Runtime functions this small are kept inlinable into the program, and so are the functions up to CALLEE_DEPTH calls
// below them that are not larger than CALLEE_INSTRUCTION_LIMIT (such as the allocator's fast paths). Larger functions are
// mostly slow paths, which LLVM would not inline anyway.
private const val HOT_INSTRUCTION_LIMIT = 64
private const val CALLEE_INSTRUCTION_LIMIT = 160
private const val CALLEE_DEPTH = 3

// Change when the way the runtime object is produced changes.
private const val CACHE_FORMAT = "kore-runtime-6"
