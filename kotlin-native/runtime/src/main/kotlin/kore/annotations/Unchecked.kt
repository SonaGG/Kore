/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

package kore.annotations

/**
 * Kore: removes the safety checks of the annotated function in optimized builds, for hot code whose indices and
 * references are known to be valid:
 *
 * - array index checks (`IndexOutOfBoundsException` of array and string accesses),
 * - null checks that throw `NullPointerException` (such as `!!`),
 * - the safepoint polls of its loops (a long loop then delays garbage collections in other threads until it ends).
 *
 * The checks of code inlined into the function (such as the array accessors) are removed too. Lambdas, local
 * functions and the functions it calls keep their checks, and so does the function wherever it is inlined into another
 * one (`inline` functions, or small functions inlined by the compiler).
 *
 * An access that a removed check would have rejected is undefined behavior: it may read or write unrelated memory.
 * Debug builds keep all checks.
 */
@Target(AnnotationTarget.FUNCTION, AnnotationTarget.CONSTRUCTOR)
@Retention(AnnotationRetention.BINARY)
@MustBeDocumented
public annotation class Unchecked
