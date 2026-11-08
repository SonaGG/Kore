/*
 * Copyright 2010-2023 JetBrains s.r.o. and Kotlin Programming Language contributors.
 * Use of this source code is governed by the Apache 2.0 license that can be found in the license/LICENSE.txt file.
 */

package org.jetbrains.kotlin.konan.target

// Kore: not built:
// - 32-bit targets: the runtime needs 64-bit pointers (compressed references, the heap reservation);
// - iOS, tvOS and watchOS: Kore targets desktops (and Linux and Android), and these would multiply the build time on macOS.
private val koreExcludedFamilies = setOf(Family.IOS, Family.TVOS, Family.WATCHOS)

fun enabledTargets(platformManager: PlatformManager) = platformManager.enabled.filterNot {
    (it in KonanTarget.deprecatedTargets && it !in KonanTarget.toleratedDeprecatedTargets) ||
        it.architecture == Architecture.X86 || it.architecture == Architecture.ARM32 || it.family in koreExcludedFamilies
}
