/*
 * Copyright 2010-2026 JetBrains s.r.o. and Kotlin Programming Language contributors.
 * Use of this source code is governed by the Apache 2.0 license that can be found in the license/LICENSE.txt file.
 */

package org.jetbrains.kotlin.utils

import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assumptions.abort

/*
Note: instead of using "kotlin.unsafe.mem.test.mode", we could just parse the "java.specification.version"
system property to check whether we run on JDK 25 or not.
But with the separate explicit property it is just more reliable, e.g.:
- We are sure that the test task does run on JDK 25 as intended.
- We don't need to parse the "java.specification.version" property value.
*/
fun getMemorySegmentMemoryAccessOrSkip(): UnsafeMemoryAccess =
    when (System.getProperty("kotlin.unsafe.mem.test.mode")) {
        "default" -> {
            assertEquals(UnsafeBasedMemoryAccess, unsafeMemoryAccess)
            abort("MemorySegment tests skipped: running in 'default' test mode")
        }
        "jdk25" -> unsafeMemoryAccess.also {
            assertEquals("MemorySegmentMemoryAccess", it.javaClass.simpleName)
        }
        else -> error("System property 'kotlin.unsafe.mem.test.mode' must be 'default' or 'jdk25'")
    }

@Suppress("JUnitTestCaseWithNoTests") // The tests are inherited from the super class.
class MemorySegmentMemoryAccessTest : UnsafeMemoryAccessTest() {
    override val memory: UnsafeMemoryAccess = getMemorySegmentMemoryAccessOrSkip()
}