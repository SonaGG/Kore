/*
 * Copyright 2010-2023 JetBrains s.r.o. and Kotlin Programming Language contributors.
 * Use of this source code is governed by the Apache 2.0 license that can be found in the license/LICENSE.txt file.
 */

package org.jetbrains.kotlin.konan.target

// Kore: the runtime needs 64-bit pointers (compressed references, the heap reservation), so 32-bit targets are not built.
fun enabledTargets(platformManager: PlatformManager) = platformManager.enabled.filterNot {
    (it in KonanTarget.deprecatedTargets && it !in KonanTarget.toleratedDeprecatedTargets) ||
        it.architecture == Architecture.X86 || it.architecture == Architecture.ARM32
}
