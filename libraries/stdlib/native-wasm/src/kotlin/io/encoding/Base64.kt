/*
 * Copyright 2010-2023 JetBrains s.r.o. and Kotlin Programming Language contributors.
 * Use of this source code is governed by the Apache 2.0 license that can be found in the license/LICENSE.txt file.
 */

package kotlin.io.encoding

@SinceKotlin("1.8")
@kotlin.internal.InlineOnly
internal actual inline fun Base64.platformCharsToBytes(source: CharSequence, startIndex: Int, endIndex: Int): ByteArray {
    // Kore: reading a String directly (rather than through `CharSequence.get`, an interface call per symbol).
    if (source is String) {
        checkSourceBounds(source.length, startIndex, endIndex)
        // The chars narrowed in one pass over the String's storage (as `charsToBytesImpl`: a symbol above 0xFF becomes an
        // illegal one).
        return narrowStringToBytes(source, startIndex, endIndex)
    }
    return charsToBytesImpl(source, startIndex, endIndex)
}


@SinceKotlin("1.8")
@kotlin.internal.InlineOnly
internal actual inline fun Base64.platformEncodeToString(source: ByteArray, startIndex: Int, endIndex: Int): String {
    val byteResult = encodeToByteArrayImpl(source, startIndex, endIndex)
    // Kore: the symbols are ASCII: the String made from them in one pass (rather than appended to a StringBuilder a char
    // at a time).
    return asciiBytesToString(byteResult)
}

@SinceKotlin("1.8")
@kotlin.internal.InlineOnly
internal actual inline fun Base64.platformEncodeIntoByteArray(
    source: ByteArray,
    destination: ByteArray,
    destinationOffset: Int,
    startIndex: Int,
    endIndex: Int
): Int {
    return encodeIntoByteArrayImpl(source, destination, destinationOffset, startIndex, endIndex)
}

@SinceKotlin("1.8")
@kotlin.internal.InlineOnly
internal actual inline fun Base64.platformEncodeToByteArray(
    source: ByteArray,
    startIndex: Int,
    endIndex: Int
): ByteArray {
    return encodeToByteArrayImpl(source, startIndex, endIndex)
}
// Kore: implemented natively (Base64Native.kt in the Kotlin/Native runtime).
internal expect fun narrowStringToBytes(string: String, startIndex: Int, endIndex: Int): ByteArray

internal expect fun asciiBytesToString(bytes: ByteArray): String
