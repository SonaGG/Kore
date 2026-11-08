/*
 * Copyright 2010-2023 JetBrains s.r.o. and Kotlin Programming Language contributors.
 * Use of this source code is governed by the Apache 2.0 license that can be found in the license/LICENSE.txt file.
 */

package org.jetbrains.kotlin.konan.target

// Kore: the runtime needs 64-bit pointers (compressed references, the heap reservation), so 32-bit targets are not built,
// nor watchos_arm64 (arm64_32: 64-bit instructions, 32-bit pointers).
fun enabledTargets(platformManager: PlatformManager) = platformManager.enabled.filterNot {
    (it in KonanTarget.deprecatedTargets && it !in KonanTarget.toleratedDeprecatedTargets) ||
        it.architecture == Architecture.X86 || it.architecture == Architecture.ARM32 || it == KonanTarget.WATCHOS_ARM64
}
