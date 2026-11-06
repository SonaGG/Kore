/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

package kotlin.collections

import kotlin.native.internal.GCUnsafeCall
import kotlin.native.internal.escapeAnalysis.Escapes

/**
 * Kore: the size (in bytes) from which `copyOfUninitializedElements` of primitive arrays copies through the runtime,
 * which does not zero the copied elements of the new array first (as allocations from the large-object cache would be).
 */
internal const val LARGE_PRIMITIVE_COPY_BYTES = 64 * 1024

/**
 * Kore: the elements `fromIndex` until `toIndex` of the primitive [array] as a new array of the same type (zero past the
 * end of [array]). Requires `0 <= fromIndex <= array.size` and `fromIndex <= toIndex`.
 */
@GCUnsafeCall("Kotlin_Array_copyOfPrimitiveRange")
@Escapes.Nothing
internal external fun copyOfPrimitiveRange(array: Any, fromIndex: Int, toIndex: Int): Any
