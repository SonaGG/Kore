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
        // The chars are copied out in bulk, then narrowed in a loop without per-char String accesses.
        val chars = source.toCharArray(startIndex, endIndex)
        val byteArray = ByteArray(chars.size)
        for (index in chars.indices) {
            val symbol = chars[index].code
            // As `charsToBytesImpl`: a symbol above 0xFF becomes an illegal one.
            byteArray[index] = if (symbol <= 0xFF) symbol.toByte() else 0x3F
        }
        return byteArray
    }
    return charsToBytesImpl(source, startIndex, endIndex)
}


@SinceKotlin("1.8")
@kotlin.internal.InlineOnly
internal actual inline fun Base64.platformEncodeToString(source: ByteArray, startIndex: Int, endIndex: Int): String {
    val byteResult = encodeToByteArrayImpl(source, startIndex, endIndex)
    // Kore: the symbols are ASCII: widened in one (vectorizable) loop and copied into the String once, rather than
    // appended to a StringBuilder a char at a time.
    val chars = CharArray(byteResult.size)
    for (i in byteResult.indices) chars[i] = (byteResult[i].toInt() and 0xFF).toChar()
    return chars.concatToString()
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