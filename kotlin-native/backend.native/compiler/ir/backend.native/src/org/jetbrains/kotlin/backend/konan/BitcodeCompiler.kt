/*
 * Copyright 2010-2018 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

package org.jetbrains.kotlin.backend.konan

import llvm.LLVMContextCreate
import llvm.LLVMContextDispose
import llvm.LLVMDisposeModule
import llvm.LLVMKotlinSplitAndRunPasses
import llvm.LLVMKotlinSplitModule
import org.jetbrains.kotlin.backend.konan.driver.NativeBackendPhaseContext
import org.jetbrains.kotlin.backend.konan.llvm.parseBitcodeFile
import org.jetbrains.kotlin.config.nativeBinaryOptions.BinaryOptions
import org.jetbrains.kotlin.konan.config.overrideClangOptions
import org.jetbrains.kotlin.konan.exec.Command
import org.jetbrains.kotlin.konan.target.*
import java.io.File
import java.util.concurrent.ExecutionException
import java.util.concurrent.Executors

typealias ObjectFile = String

internal class BitcodeCompiler(
    private val context: NativeBackendPhaseContext,
) {

    private val config = context.config
    private val platform = config.platform
    private val optimize = context.shouldOptimize()
    private val debug = config.debug

    private val overrideClangOptions =
            config.configuration.overrideClangOptions

    private fun MutableList<String>.addNonEmpty(elements: List<String>) {
        addAll(elements.filter { it.isNotEmpty() })
    }

    private fun runTool(vararg command: String) =
            Command(*command)
                    .logWith(context::log)
                    .execute()

    private fun targetTool(tool: String, vararg arg: String) {
        val absoluteToolName = "${platform.absoluteTargetToolchain}/bin/$tool"
        runTool(absoluteToolName, *arg)
    }

    private fun hostLlvmTool(tool: String, vararg arg: String) {
        val absoluteToolName = "${platform.absoluteLlvmHome}/bin/$tool"
        runTool(absoluteToolName, *arg)
    }

    private fun clang(configurables: ClangFlags, bitcodeFile: File, objectFile: File) {
        val targetTriple = if (configurables is AppleConfigurables) {
            platform.targetTriple.withOSVersion(configurables.osVersionMin)
        } else {
            platform.targetTriple
        }
        val flags = overrideClangOptions.takeIf(List<String>::isNotEmpty)
                ?: mutableListOf<String>().apply {
                    addNonEmpty(configurables.clangFlags)
                    addNonEmpty(listOf("-triple", targetTriple.toString()))
                    addNonEmpty(when {
                        optimize -> configurables.clangOptFlags
                        debug -> configurables.clangDebugFlags
                        else -> configurables.clangNooptFlags
                    })
                    addNonEmpty(configurables.currentRelocationMode(context).translateToClangCc1Flag())
                }
        val bitcodePath = bitcodeFile.absoluteFile.normalize().path
        val objectPath = objectFile.absoluteFile.normalize().path
        if (configurables is AppleConfigurables && config.configuration.get(BinaryOptions.compileBitcodeWithXcodeLlvm) == true) {
            targetTool("clang++", *flags.toTypedArray(), bitcodePath, "-o", objectPath)
        } else {
            hostLlvmTool("clang++", *flags.toTypedArray(), bitcodePath, "-o", objectPath)
        }
    }

    private fun RelocationModeFlags.Mode.translateToClangCc1Flag() = when (this) {
        RelocationModeFlags.Mode.PIC -> listOf("-mrelocation-model", "pic")
        RelocationModeFlags.Mode.STATIC -> listOf("-mrelocation-model", "static")
        RelocationModeFlags.Mode.DEFAULT -> emptyList()
    }

    /**
     * Compile [bitcodeFile] to [objectFile].
     */
    fun makeObjectFile(bitcodeFile: File, objectFile: File) =
            when (val configurables = platform.configurables) {
                is ClangFlags -> clang(configurables, bitcodeFile, objectFile)
                else -> error("Unsupported configurables kind: ${configurables::class.simpleName}!")
            }

    /**
     * Kore: compile [bitcodeFile] to object files. Large optimized executables are split into partitions which are
     * compiled to machine code by parallel clang processes; otherwise this is [makeObjectFile] into [objectFile].
     */
    fun makeObjectFiles(bitcodeFile: File, objectFile: File): List<File> {
        val configurables = platform.configurables
        val ltoPartitions = config.parallelLtoPartitions
        val partitions = when {
            ltoPartitions > 1 -> ltoPartitions
            configurables is ClangFlags -> codegenPartitions(configurables, bitcodeFile)
            else -> 1
        }
        if (partitions <= 1) {
            makeObjectFile(bitcodeFile, objectFile)
            return listOf(objectFile)
        }
        configurables as ClangFlags
        val prefix = objectFile.absoluteFile.normalize().path + ".part"
        val written = if (ltoPartitions > 1) {
            splitAndOptimize(bitcodeFile, partitions, prefix).also {
                check(it > 0) { "Failed to optimize the partitions of $bitcodeFile" }
            }
        } else {
            splitBitcode(bitcodeFile, partitions, prefix)
        }
        if (written <= 0) {
            makeObjectFile(bitcodeFile, objectFile)
            return listOf(objectFile)
        }
        val parts = (0 until written).map { File("$prefix$it.bc") to File("$prefix$it.o") }
        val executor = Executors.newFixedThreadPool(parts.size)
        try {
            parts.map { part -> executor.submit { clang(configurables, part.first, part.second) } }.forEach {
                try {
                    it.get()
                } catch (e: ExecutionException) {
                    throw e.cause ?: e
                }
            }
        } finally {
            executor.shutdownNow()
        }
        parts.forEach { it.first.delete() }
        return parts.map { it.second }
    }

    private fun codegenPartitions(configurables: ClangFlags, bitcodeFile: File): Int {
        val requested = config.codegenPartitions.toInt()
        if (requested == 1 || !optimize) return 1
        // Splitting would externalize the instrumentation's counters and profile data, which belong to COMDAT groups.
        // Instrumented binaries are only for training, so their compile time matters less.
        if (config.pgoGenerate) return 1
        // Externalized locals would be exported from a DLL by MinGW's automatic export, and caches and libraries are
        // linked into other binaries, so only split executables.
        if (config.produce != CompilerOutputKind.PROGRAM) return 1
        if (configurables is AppleConfigurables && config.configuration.get(BinaryOptions.compileBitcodeWithXcodeLlvm) == true) return 1
        if (requested > 1) return requested
        if (bitcodeFile.length() < MIN_SPLIT_BITCODE_BYTES) return 1
        return Runtime.getRuntime().availableProcessors().coerceIn(1, MAX_AUTO_PARTITIONS)
    }

    // Kore: see [parallelLtoPartitions]. The module has been through the LTO pre-link pipeline and internalized.
    private fun splitAndOptimize(bitcodeFile: File, partitions: Int, prefix: String): Int {
        val pipelineConfig = createLTOFinalPipelineConfig(context, platform.targetTriple.toString(), closedWorld = config.isFinalBinary)
        val passes = LTOPreSplitPipeline(pipelineConfig, null).use { it.partitionPasses }
        LlvmOptimizationPipeline.initLLVMOnce()
        val llvmContext = LLVMContextCreate()!!
        try {
            val module = parseBitcodeFile(context, context.diagnosticReporter, llvmContext, bitcodeFile.absoluteFile.normalize().path)
            try {
                return LLVMKotlinSplitAndRunPasses(
                        module, partitions, passes, pipelineConfig.targetTriple, pipelineConfig.cpuModel, pipelineConfig.cpuFeatures,
                        pipelineConfig.codegenOptimizationLevel, pipelineConfig.relocMode, pipelineConfig.codeModel,
                        pipelineConfig.inlineThreshold ?: -1, prefix,
                )
            } finally {
                LLVMDisposeModule(module)
            }
        } finally {
            LLVMContextDispose(llvmContext)
        }
    }

    private fun splitBitcode(bitcodeFile: File, partitions: Int, prefix: String): Int {
        val llvmContext = LLVMContextCreate()!!
        try {
            val module = parseBitcodeFile(context, context.diagnosticReporter, llvmContext, bitcodeFile.absoluteFile.normalize().path)
            try {
                return LLVMKotlinSplitModule(module, partitions, prefix)
            } finally {
                LLVMDisposeModule(module)
            }
        } finally {
            LLVMContextDispose(llvmContext)
        }
    }

    private companion object {
        const val MIN_SPLIT_BITCODE_BYTES = 256L * 1024
        const val MAX_AUTO_PARTITIONS = 8
    }
}
