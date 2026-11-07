/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

package kotlin.io.encoding

import kotlin.native.internal.GCUnsafeCall
import kotlin.native.internal.escapeAnalysis.Escapes

/**
 * Kore: the chars [startIndex] until [endIndex] of [string] as bytes, a char above 0xFF becoming '?' (an illegal Base64
 * symbol), in one pass over the string's own storage (see `Base64.platformCharsToBytes`).
 */
@GCUnsafeCall("Kotlin_String_narrowToBytes")
@Escapes.Nothing
internal actual external fun narrowStringToBytes(string: String, startIndex: Int, endIndex: Int): ByteArray

/** Kore: the ASCII bytes [bytes] as a String, in one pass (see `Base64.platformEncodeToString`). */
@GCUnsafeCall("Kotlin_ByteArray_asciiToString")
@Escapes.Nothing
internal actual external fun asciiBytesToString(bytes: ByteArray): String
