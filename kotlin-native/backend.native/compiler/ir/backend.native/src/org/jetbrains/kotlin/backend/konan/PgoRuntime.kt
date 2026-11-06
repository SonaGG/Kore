/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

package org.jetbrains.kotlin.backend.konan

import org.jetbrains.kotlin.backend.konan.driver.NativeBackendPhaseContext
import org.jetbrains.kotlin.konan.TempFiles
import org.jetbrains.kotlin.konan.library.javaFile
import org.jetbrains.kotlin.konan.properties.KonanPropertiesLoader
import org.jetbrains.kotlin.konan.target.HostManager
import org.jetbrains.kotlin.konan.target.KonanTarget
import org.jetbrains.kotlin.konan.target.LinkerOutputKind
import java.io.File

/**
 * Kore: what a binary instrumented for PGO (see [NativeSecondStageCompilationConfig.pgoGenerate]) additionally links:
 * LLVM's profile runtime, which writes the raw profile when the program exits.
 */
internal class PgoRuntimeLinkage(val objectFiles: List<String>, val linkerArgs: List<String>)

internal fun pgoRuntimeLinkage(context: NativeBackendPhaseContext, tempFiles: TempFiles, linkerOutput: LinkerOutputKind): PgoRuntimeLinkage? {
    val config = context.config
    // A static library is linked into a program by another toolchain, which provides the profile runtime.
    if (!config.pgoGenerate || linkerOutput == LinkerOutputKind.STATIC_LIBRARY) return null
    if (config.target != KonanTarget.MINGW_X64) {
        context.reportCompilationError("Binary option pgoGenerate is not supported for ${config.target.visibleName} yet")
    }
    val configurables = config.platform.configurables
    // The LLVM distribution used to compile does not necessarily include compiler-rt, but its development
    // distribution (for building the compiler) does.
    val devLlvm = (configurables as? KonanPropertiesLoader)?.properties?.getProperty("llvm.${HostManager.host.name}.dev")
    // Not a declared dependency of the target, so look for it next to the LLVM distribution in use.
    val llvmHome = File(configurables.absoluteLlvmHome)
    val llvmHomes = listOfNotNull(llvmHome.path, devLlvm?.let { File(llvmHome.parentFile, it).path }).distinct()
    val compilerRtDirs = llvmHomes.flatMap { home ->
        File(home, "lib/clang").listFiles().orEmpty().sortedDescending().map { File(it, "lib/windows") }
    }
    for (dir in compilerRtDirs) {
        // Built for MinGW: links as is.
        File(dir, "libclang_rt.profile-x86_64.a").takeIf { it.isFile }?.let {
            return PgoRuntimeLinkage(listOf(it.absolutePath), emptyList())
        }
        // Built for MSVC: does not need its C runtime other than the few functions MinGW lacks, see [MSVC_PROFILE_RUNTIME_SHIM].
        File(dir, "clang_rt.profile-x86_64.lib").takeIf { it.isFile }?.let {
            val shimSource = tempFiles.create("pgo_runtime_shim", ".ll").javaFile()
            val shimObject = tempFiles.create("pgo_runtime_shim", ".o").javaFile()
            shimSource.writeText(MSVC_PROFILE_RUNTIME_SHIM)
            BitcodeCompiler(context).makeObjectFile(shimSource, shimObject)
            return PgoRuntimeLinkage(
                    listOf(shimObject.absolutePath, it.absolutePath),
                    listOf("-Wl,-Xlink=-nodefaultlib:libcmt.lib", "-Wl,-Xlink=-nodefaultlib:oldnames.lib"),
            )
        }
    }
    context.reportCompilationError(
            "Binary option pgoGenerate needs LLVM's profile runtime (clang_rt.profile), which was not found in ${llvmHomes.joinToString()}. " +
                    "It is part of the LLVM development distribution${devLlvm?.let { " $it" }.orEmpty()}: compiling once with " +
                    "-Xoverride-konan-properties=llvmHome.${config.target.name}=${devLlvm ?: "<LLVM development distribution>"} downloads it."
    )
}

/**
 * The profile runtime of LLVM's Windows distribution is built against the Universal C runtime and with MSVC's stack
 * checks; MinGW's C runtime (msvcrt) lacks these functions.
 */
private val MSVC_PROFILE_RUNTIME_SHIM = """
    module asm ".globl __chkstk"
    module asm "__chkstk:"
    module asm "  jmp ___chkstk_ms"

    declare i32 @__mingw_vfprintf(ptr, ptr, ptr)
    declare i32 @__mingw_vsnprintf(ptr, i64, ptr, ptr)

    define i32 @__stdio_common_vfprintf(i64 %options, ptr %stream, ptr %format, ptr %locale, ptr %args) {
      %result = call i32 @__mingw_vfprintf(ptr %stream, ptr %format, ptr %args)
      ret i32 %result
    }

    ; With option _CRT_INTERNAL_PRINTF_STANDARD_SNPRINTF_BEHAVIOR (2) like C99 vsnprintf, otherwise -1 when truncated.
    define i32 @__stdio_common_vsprintf(i64 %options, ptr %buffer, i64 %count, ptr %format, ptr %locale, ptr %args) {
      %result = call i32 @__mingw_vsnprintf(ptr %buffer, i64 %count, ptr %format, ptr %args)
      %standardOption = and i64 %options, 2
      %standard = icmp ne i64 %standardOption, 0
      %result64 = sext i32 %result to i64
      %fits = icmp slt i64 %result64, %count
      %ok = or i1 %standard, %fits
      %legacyResult = select i1 %ok, i32 %result, i32 -1
      ret i32 %legacyResult
    }

    ; The profile runtime is not hardened here.
    define void @__security_check_cookie(i64 %cookie) {
      ret void
    }
""".trimIndent()
