/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

package kotlin.collections

/**
 * Kore: [HashMap] specialized for `Int` keys and `Int` values, which it keeps in `IntArray`s instead of boxing them.
 *
 * The compiler allocates it in place of `HashMap<Int, Int>` where it sees every use of the map (see
 * `PrimitiveCollections` in the backend), and calls [putInt], [getOrMissing] and friends, which take and return
 * primitives. It behaves exactly like [HashMap] (the same insertion-ordered layout, compaction, views and
 * `toString`/`equals`/`hashCode`), and has the same erased signatures, so that keys or values of other types are
 * handled as [HashMap] does (not found, not equal).
 */
@Suppress("UNCHECKED_CAST")
internal class IntIntHashMap<K, V>(initialCapacity: Int) : MutableMap<K, V> {
    init {
        // Before the arrays are allocated, as [HashMap] checks.
        require(initialCapacity >= 0) { "capacity must be non-negative." }
    }

    // Keys and values in insertion order.
    private var keysArray = IntArray(initialCapacity)
    private var valuesArray = IntArray(initialCapacity)
    // The slot of the key at each index, TOMBSTONE if the key at that index was removed.
    private var presenceArray = IntArray(initialCapacity)
    // (index + 1) of a key by its slot, 0 if there is no key in that slot.
    private var hashArray = IntArray(computeHashSize(initialCapacity))
    private var maxProbeDistance = INITIAL_MAX_PROBE_DISTANCE
    private var length = 0
    private var hashShift = computeShift(hashSize)
    // As in [HashMap]: the low bits of the keys, until probe sequences get long, then Fibonacci hashing.
    private var hashMultiplier = 1
    private var hashLeftShift = hashShift
    private var modCount = 0
    private var _size = 0

    private var keysView: MutableSet<K>? = null
    private var valuesView: MutableCollection<V>? = null
    private var entriesView: MutableSet<MutableMap.MutableEntry<K, V>>? = null

    constructor() : this(INITIAL_CAPACITY)

    override val size: Int get() = _size

    // ---------------------------- primitive entry points ----------------------------

    /** The value of [key] as a `Long`, or [MISSING] if there is none. */
    fun getOrMissing(key: Int): Long {
        val index = findKey(key)
        return if (index < 0) MISSING else valuesArray[index].toLong()
    }

    fun containsKeyInt(key: Int): Boolean = findKey(key) >= 0

    /** Puts [value] for [key]; returns the previous value as a `Long`, or [MISSING]. */
    @IgnorableReturnValue
    fun putInt(key: Int, value: Int): Long {
        val index = addKey(key)
        if (index < 0) {
            val old = valuesArray[-index - 1]
            valuesArray[-index - 1] = value
            return old.toLong()
        }
        valuesArray[index] = value
        return MISSING
    }

    /** Removes [key]; returns its value as a `Long`, or [MISSING]. */
    @IgnorableReturnValue
    fun removeInt(key: Int): Long {
        val index = findKey(key)
        if (index < 0) return MISSING
        val old = valuesArray[index]
        removeEntryAt(index)
        return old.toLong()
    }

    // ---------------------------- MutableMap ----------------------------

    override fun isEmpty(): Boolean = _size == 0
    override fun containsKey(key: K): Boolean = key is Int && findKey(key) >= 0
    override fun containsValue(value: V): Boolean = value is Int && findValue(value) >= 0

    override operator fun get(key: K): V? {
        if (key !is Int) return null
        val index = findKey(key)
        if (index < 0) return null
        return valuesArray[index] as V
    }

    @IgnorableReturnValue
    override fun put(key: K, value: V): V? {
        val old = putInt(key as Int, value as Int)
        return if (old == MISSING) null else old.toInt() as V
    }

    override fun putAll(from: Map<out K, V>) {
        val entries = from.entries
        if (entries.isEmpty()) return
        ensureExtraCapacity(entries.size)
        for (entry in entries) putInt(entry.key as Int, entry.value as Int)
    }

    @IgnorableReturnValue
    override fun remove(key: K): V? {
        if (key !is Int) return null
        val old = removeInt(key)
        return if (old == MISSING) null else old.toInt() as V
    }

    override fun clear() {
        for (i in 0 until length) {
            val hash = presenceArray[i]
            if (hash >= 0) {
                hashArray[hash] = 0
                presenceArray[i] = TOMBSTONE
            }
        }
        _size = 0
        length = 0
        registerModification()
    }

    override val keys: MutableSet<K>
        get() = keysView ?: Keys(this).also { keysView = it }

    override val values: MutableCollection<V>
        get() = valuesView ?: Values(this).also { valuesView = it }

    override val entries: MutableSet<MutableMap.MutableEntry<K, V>>
        get() = entriesView ?: Entries(this).also { entriesView = it }

    override fun equals(other: Any?): Boolean =
            other === this || (other is Map<*, *> && _size == other.size && containsAllEntries(other.entries))

    override fun hashCode(): Int {
        var result = 0
        for (i in 0 until length) if (presenceArray[i] >= 0) result += keysArray[i] xor valuesArray[i]
        return result
    }

    override fun toString(): String {
        val sb = StringBuilder(2 + _size * 3)
        sb.append("{")
        var first = true
        for (i in 0 until length) {
            if (presenceArray[i] < 0) continue
            if (!first) sb.append(", ")
            first = false
            sb.append(keysArray[i]).append('=').append(valuesArray[i])
        }
        sb.append("}")
        return sb.toString()
    }

    // ---------------------------- private ----------------------------

    private val capacity: Int get() = keysArray.size
    private val hashSize: Int get() = hashArray.size

    private fun registerModification() {
        modCount += 1
    }

    private fun ensureExtraCapacity(n: Int) {
        if (shouldCompact(extraCapacity = n)) {
            compact(updateHashArray = true)
        } else {
            ensureCapacity(length + n)
        }
    }

    private fun shouldCompact(extraCapacity: Int): Boolean {
        val spareCapacity = capacity - length
        val gaps = length - size
        return spareCapacity < extraCapacity && gaps + spareCapacity >= extraCapacity && gaps >= capacity / 4
    }

    private fun ensureCapacity(minCapacity: Int) {
        if (minCapacity < 0) throw OutOfMemoryError()    // overflow
        if (minCapacity > capacity) {
            val newSize = AbstractList.newCapacity(capacity, minCapacity.coerceAtLeast(capacity * 2).let {
                if (it < 0) minCapacity else it
            })
            keysArray = keysArray.copyOf(newSize)
            valuesArray = valuesArray.copyOf(newSize)
            presenceArray = presenceArray.copyOf(newSize)
            val newHashSize = computeHashSize(newSize)
            if (newHashSize > hashSize) rehash(newHashSize)
        }
    }

    private fun slotOf(key: Int): Int = ((key * hashMultiplier) shl hashLeftShift) ushr hashShift

    private val isLowBitsHashing: Boolean get() = hashMultiplier == 1

    private fun switchToFibonacciHashing() {
        hashMultiplier = MAGIC
        hashLeftShift = 0
        maxProbeDistance = INITIAL_MAX_PROBE_DISTANCE
    }

    private fun compact(updateHashArray: Boolean) {
        var i = 0
        var j = 0
        while (i < length) {
            val hash = presenceArray[i]
            if (hash >= 0) {
                keysArray[j] = keysArray[i]
                valuesArray[j] = valuesArray[i]
                if (updateHashArray) {
                    presenceArray[j] = hash
                    hashArray[hash] = j + 1
                }
                j++
            }
            i++
        }
        length = j
    }

    private fun rehash(newHashSize: Int) {
        registerModification()
        if (length > _size) compact(updateHashArray = false)
        hashShift = computeShift(newHashSize)
        if (isLowBitsHashing) hashLeftShift = hashShift
        while (true) {
            hashArray = IntArray(newHashSize)
            var i = 0
            while (i < length) putRehash(i++)
            if (!isLowBitsHashing || maxProbeDistance <= MAX_LOW_BITS_PROBE_DISTANCE) return
            switchToFibonacciHashing()
        }
    }

    private fun putRehash(i: Int) {
        var hash = slotOf(keysArray[i])
        var probeDistance = 0
        while (true) {
            if (hashArray[hash] == 0) {
                hashArray[hash] = i + 1
                presenceArray[i] = hash
                if (probeDistance > maxProbeDistance) maxProbeDistance = probeDistance
                return
            }
            probeDistance++
            if (hash-- == 0) hash = hashSize - 1
        }
    }

    private fun findKey(key: Int): Int {
        var hash = slotOf(key)
        var probesLeft = maxProbeDistance
        while (true) {
            val index = hashArray[hash]
            if (index == 0) return TOMBSTONE
            if (keysArray[index - 1] == key) return index - 1
            if (--probesLeft < 0) return TOMBSTONE
            if (hash-- == 0) hash = hashSize - 1
        }
    }

    private fun findValue(value: Int): Int {
        var i = length
        while (--i >= 0) {
            if (presenceArray[i] >= 0 && valuesArray[i] == value) return i
        }
        return TOMBSTONE
    }

    private fun addKey(key: Int): Int {
        retry@ while (true) {
            var hash = slotOf(key)
            var tentativeMaxProbeDistance = (maxProbeDistance * 2).coerceAtMost(hashSize / 2)
            if (isLowBitsHashing && tentativeMaxProbeDistance > MAX_LOW_BITS_PROBE_DISTANCE)
                tentativeMaxProbeDistance = MAX_LOW_BITS_PROBE_DISTANCE
            var probeDistance = 0
            while (true) {
                val index = hashArray[hash]
                if (index == 0) {
                    if (length >= capacity) {
                        ensureExtraCapacity(1)
                        continue@retry
                    }
                    val putIndex = length++
                    keysArray[putIndex] = key
                    presenceArray[putIndex] = hash
                    hashArray[hash] = putIndex + 1
                    _size++
                    registerModification()
                    if (probeDistance > maxProbeDistance) maxProbeDistance = probeDistance
                    return putIndex
                }
                if (keysArray[index - 1] == key) return -index
                if (++probeDistance > tentativeMaxProbeDistance) {
                    if (isLowBitsHashing) {
                        switchToFibonacciHashing()
                        rehash(hashSize)
                    } else {
                        rehash(hashSize * 2)
                    }
                    continue@retry
                }
                if (hash-- == 0) hash = hashSize - 1
            }
        }
    }

    private fun removeEntryAt(index: Int) {
        removeHashAt(presenceArray[index])
        presenceArray[index] = TOMBSTONE
        _size--
        registerModification()
    }

    private fun removeHashAt(removedHash: Int) {
        var hash = removedHash
        var hole = removedHash
        var probeDistance = 0
        while (true) {
            if (hash-- == 0) hash = hashSize - 1
            val index = hashArray[hash]
            if (++probeDistance > maxProbeDistance) {
                hashArray[hole] = 0
                return
            }
            if (index == 0) {
                hashArray[hole] = 0
                return
            }
            val otherHash = slotOf(keysArray[index - 1])
            if ((otherHash - hash) and (hashSize - 1) >= probeDistance) {
                hashArray[hole] = index
                presenceArray[index - 1] = hole
                hole = hash
                probeDistance = 0
            }
        }
    }

    private fun containsEntry(entry: Map.Entry<*, *>): Boolean {
        val key = entry.key
        val value = entry.value
        if (key !is Int || value !is Int) return false
        val index = findKey(key)
        return index >= 0 && valuesArray[index] == value
    }

    private fun containsAllEntries(m: Collection<*>): Boolean {
        for (entry in m) {
            if (entry !is Map.Entry<*, *> || !containsEntry(entry)) return false
        }
        return true
    }

    private fun removeEntry(entry: Map.Entry<*, *>): Boolean {
        val key = entry.key
        val value = entry.value
        if (key !is Int || value !is Int) return false
        val index = findKey(key)
        if (index < 0 || valuesArray[index] != value) return false
        removeEntryAt(index)
        return true
    }

    private fun removeValue(value: Any?): Boolean {
        if (value !is Int) return false
        val index = findValue(value)
        if (index < 0) return false
        removeEntryAt(index)
        return true
    }

    private open class Itr<K, V>(val map: IntIntHashMap<K, V>) {
        var index = 0
        var lastIndex = -1
        private var expectedModCount = map.modCount

        init {
            initNext()
        }

        fun initNext() {
            while (index < map.length && map.presenceArray[index] < 0) index++
        }

        fun hasNext(): Boolean = index < map.length

        fun advance(): Int {
            checkForComodification()
            if (index >= map.length) throw NoSuchElementException()
            lastIndex = index++
            initNext()
            return lastIndex
        }

        fun remove() {
            checkForComodification()
            check(lastIndex != -1) { "Call next() before removing element from the iterator." }
            map.removeEntryAt(lastIndex)
            lastIndex = -1
            expectedModCount = map.modCount
        }

        fun checkForComodification() {
            if (map.modCount != expectedModCount) throw ConcurrentModificationException()
        }
    }

    private class KeysItr<K, V>(map: IntIntHashMap<K, V>) : Itr<K, V>(map), MutableIterator<K> {
        override fun next(): K = map.keysArray[advance()] as K
    }

    private class ValuesItr<K, V>(map: IntIntHashMap<K, V>) : Itr<K, V>(map), MutableIterator<V> {
        override fun next(): V = map.valuesArray[advance()] as V
    }

    private class EntriesItr<K, V>(map: IntIntHashMap<K, V>) : Itr<K, V>(map), MutableIterator<MutableMap.MutableEntry<K, V>> {
        override fun next(): MutableMap.MutableEntry<K, V> = EntryRef(map, advance())
    }

    private class EntryRef<K, V>(private val map: IntIntHashMap<K, V>, private val index: Int) : MutableMap.MutableEntry<K, V> {
        private val expectedModCount = map.modCount

        override val key: K
            get() {
                checkForComodification()
                return map.keysArray[index] as K
            }

        override val value: V
            get() {
                checkForComodification()
                return map.valuesArray[index] as V
            }

        override fun setValue(newValue: V): V {
            checkForComodification()
            val old = map.valuesArray[index]
            map.valuesArray[index] = newValue as Int
            return old as V
        }

        override fun equals(other: Any?): Boolean = other is Map.Entry<*, *> && other.key == key && other.value == value
        override fun hashCode(): Int = key.hashCode() xor value.hashCode()
        override fun toString(): String = "$key=$value"

        private fun checkForComodification() {
            if (map.modCount != expectedModCount)
                throw ConcurrentModificationException("The backing map has been modified after this entry was obtained.")
        }
    }

    private class Keys<K, V>(private val backing: IntIntHashMap<K, V>) : AbstractMutableSet<K>() {
        override val size: Int get() = backing.size
        override fun isEmpty(): Boolean = backing.isEmpty()
        override fun contains(element: K): Boolean = backing.containsKey(element)
        override fun clear() = backing.clear()
        override fun add(element: K): Boolean = throw UnsupportedOperationException()
        override fun addAll(elements: Collection<K>): Boolean = throw UnsupportedOperationException()
        override fun remove(element: K): Boolean = element is Int && backing.removeInt(element) != MISSING
        override fun iterator(): MutableIterator<K> = KeysItr(backing)
    }

    private class Values<K, V>(private val backing: IntIntHashMap<K, V>) : AbstractMutableCollection<V>() {
        override val size: Int get() = backing.size
        override fun isEmpty(): Boolean = backing.isEmpty()
        override fun contains(element: V): Boolean = backing.containsValue(element)
        override fun add(element: V): Boolean = throw UnsupportedOperationException()
        override fun addAll(elements: Collection<V>): Boolean = throw UnsupportedOperationException()
        override fun clear() = backing.clear()
        override fun iterator(): MutableIterator<V> = ValuesItr(backing)
        override fun remove(element: V): Boolean = backing.removeValue(element)
    }

    private class Entries<K, V>(private val backing: IntIntHashMap<K, V>) : AbstractMutableSet<MutableMap.MutableEntry<K, V>>() {
        override val size: Int get() = backing.size
        override fun isEmpty(): Boolean = backing.isEmpty()
        override fun contains(element: MutableMap.MutableEntry<K, V>): Boolean = backing.containsEntry(element)
        override fun containsAll(elements: Collection<MutableMap.MutableEntry<K, V>>): Boolean = backing.containsAllEntries(elements)
        override fun clear() = backing.clear()
        override fun add(element: MutableMap.MutableEntry<K, V>): Boolean = throw UnsupportedOperationException()
        override fun addAll(elements: Collection<MutableMap.MutableEntry<K, V>>): Boolean = throw UnsupportedOperationException()
        override fun remove(element: MutableMap.MutableEntry<K, V>): Boolean = backing.removeEntry(element)
        override fun iterator(): MutableIterator<MutableMap.MutableEntry<K, V>> = EntriesItr(backing)
    }

    internal companion object {
        /** Returned by the primitive entry points for a missing key (no `Int` value maps to it). */
        const val MISSING = Long.MIN_VALUE
        private const val MAGIC = -1640531527
        private const val INITIAL_CAPACITY = 8
        private const val INITIAL_MAX_PROBE_DISTANCE = 2
        private const val MAX_LOW_BITS_PROBE_DISTANCE = 8
        private const val TOMBSTONE = -1

        private fun computeHashSize(capacity: Int): Int = (capacity.coerceAtLeast(1) * 3).takeHighestOneBit()
        private fun computeShift(hashSize: Int): Int = hashSize.countLeadingZeroBits() + 1
    }
}
