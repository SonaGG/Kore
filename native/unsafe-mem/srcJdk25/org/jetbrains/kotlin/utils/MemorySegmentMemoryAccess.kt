/*
 * Copyright 2010-2026 JetBrains s.r.o. and Kotlin Programming Language contributors.
 * Use of this source code is governed by the Apache 2.0 license that can be found in the license/LICENSE.txt file.
 */

package org.jetbrains.kotlin.utils

import java.lang.foreign.*
import java.lang.foreign.ValueLayout.*
import java.lang.invoke.MethodHandle

private object MemorySegmentMemoryAccess : UnsafeMemoryAccess {
    /**
     * A memory segment that seemingly covers the entire address space.
     * So, all memory accesses are routed through it, with addresses used as offsets within this segment.
     *
     * The problem is that `Long.MAX_VALUE` doesn't actually cover the entire address space
     * but only roughly a half of it. This should work in practice so far, but might become problematic
     * in the future.
     *
     * An alternative would be to use properly scoped [MemorySegment]s, i.e. implement each memory access like
     * `MemorySegment.ofAddress(address).reinterpret(4).get(ValueLayout.JAVA_INT, 0)`.
     * This might have performance implications in the current setup.
     *
     * See KT-85209 for details.
     */
    private val memory: MemorySegment = MemorySegment.ofAddress(0L).reinterpret(Long.MAX_VALUE)

    private val malloc: MethodHandle
    private val free: MethodHandle

    init {
        val linker = Linker.nativeLinker()
        val lookup = linker.defaultLookup()

        check(ADDRESS.byteSize() == 8L) { "Expected address size to be 8 bytes, but got ${ADDRESS.byteSize()}" }
        // So the size_t parameter of `malloc` can be safely assumed to be 64-bit => JAVA_LONG can be used.
        malloc = linker.downcallHandle(
            lookup.find("malloc").orElseThrow(),
            FunctionDescriptor.of(ADDRESS, JAVA_LONG),
        )
        free = linker.downcallHandle(
            lookup.find("free").orElseThrow(),
            FunctionDescriptor.ofVoid(ADDRESS),
        )
    }

    override fun allocateMemory(size: Long): Long {
        require(size > 0) { "allocateMemory size must be positive, but was $size" }
        val result = malloc.invoke(size) as MemorySegment
        if (result.address() == 0L) throw OutOfMemoryError("malloc($size) returned null")
        return result.address()
    }

    override fun freeMemory(address: Long) {
        free.invoke(MemorySegment.ofAddress(address))
    }

    override fun getByte(address: Long): Byte = memory.get(JAVA_BYTE, address)
    override fun putByte(address: Long, value: Byte) = memory.set(JAVA_BYTE, address, value)

    override fun getShort(address: Long): Short = memory.get(JAVA_SHORT_UNALIGNED, address)
    override fun putShort(address: Long, value: Short) = memory.set(JAVA_SHORT_UNALIGNED, address, value)

    override fun getInt(address: Long): Int = memory.get(JAVA_INT_UNALIGNED, address)
    override fun putInt(address: Long, value: Int) = memory.set(JAVA_INT_UNALIGNED, address, value)

    override fun getLong(address: Long): Long = memory.get(JAVA_LONG_UNALIGNED, address)
    override fun putLong(address: Long, value: Long) = memory.set(JAVA_LONG_UNALIGNED, address, value)

    override fun getFloat(address: Long): Float = memory.get(JAVA_FLOAT_UNALIGNED, address)
    override fun putFloat(address: Long, value: Float) = memory.set(JAVA_FLOAT_UNALIGNED, address, value)

    override fun getDouble(address: Long): Double = memory.get(JAVA_DOUBLE_UNALIGNED, address)
    override fun putDouble(address: Long, value: Double) = memory.set(JAVA_DOUBLE_UNALIGNED, address, value)

    override fun zeroMemory(address: Long, length: Long) {
        memory.asSlice(address, length).fill(0)
    }

    override fun copyFromByteArray(src: ByteArray, destAddress: Long, length: Int) {
        MemorySegment.copy(src, 0, memory, JAVA_BYTE, destAddress, length)
    }

    override fun copyToByteArray(srcAddress: Long, dest: ByteArray, length: Int) {
        MemorySegment.copy(memory, JAVA_BYTE, srcAddress, dest, 0, length)
    }

    override fun copyFromCharArray(src: CharArray, destAddress: Long, lengthInChars: Int) {
        MemorySegment.copy(src, 0, memory, JAVA_CHAR_UNALIGNED, destAddress, lengthInChars)
    }
}

/**
 * When running on JDK 25+, this class shadows the default implementation defined in `UnsafeBasedMemoryAccess.kt`.
 * It achieved using the multi-release JAR files feature.
 * This allows selecting the implementation of [UnsafeMemoryAccess] based on the JDK version at runtime.
 *
 * It is important to keep both classes in sync.
 */
@Suppress("unused")
internal object UnsafeMemoryAccessProvider {
    fun getImplementation(): UnsafeMemoryAccess = MemorySegmentMemoryAccess
}
