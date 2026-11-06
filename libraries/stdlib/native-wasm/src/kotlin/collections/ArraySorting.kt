/*
 * Copyright 2010-2018 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

package kotlin.collections


private fun <T: Comparable<T>> mergeSort(array: Array<T>, start: Int, endInclusive: Int) {
    @Suppress("UNCHECKED_CAST")
    val buffer = arrayOfNulls<Any?>(array.size) as Array<T>
    val result = mergeSort(array, buffer, start, endInclusive)
    if (result !== array) {
        for (i in start..endInclusive) array[i] = result[i]
    }
}

// Both start and end are inclusive indices.
private fun <T: Comparable<T>> mergeSort(array: Array<T>, buffer: Array<T>, start: Int, end: Int): Array<T> {
    if (start == end) {
        return array
    }

    val median = (start + end) / 2
    val left = mergeSort(array, buffer, start, median)
    val right = mergeSort(array, buffer, median + 1, end)

    val target = if (left === buffer) array else buffer

    // Merge.
    var leftIndex = start
    var rightIndex = median + 1
    for (i in start..end) {
        when {
            leftIndex <= median && rightIndex <= end -> {
                val leftValue = left[leftIndex]
                val rightValue = right[rightIndex]

                if (leftValue <= rightValue) {
                    target[i] = leftValue
                    leftIndex++
                } else {
                    target[i] = rightValue
                    rightIndex++
                }
            }
            leftIndex <= median -> {
                target[i] = left[leftIndex]
                leftIndex++
            }
            else /* rightIndex <= end */ -> {
                target[i] = right[rightIndex]
                rightIndex++
            }
        }
    }

    return target
}

// Sort with comparator.
private fun <T> mergeSort(array: Array<T>, start: Int, endInclusive: Int, comparator: Comparator<T>) {
    @Suppress("UNCHECKED_CAST")
    val buffer = arrayOfNulls<Any?>(array.size) as Array<T>
    val result = mergeSort(array, buffer, start, endInclusive, comparator)
    if (result !== array) {
        for (i in start..endInclusive) array[i] = result[i]
    }
}

// Both start and end are inclusive indices.
private fun <T> mergeSort(array: Array<T>, buffer: Array<T>, start: Int, end: Int, comparator: Comparator<T>): Array<T> {
    if (start == end) {
        return array
    }

    val median = (start + end) / 2
    val left = mergeSort(array, buffer, start, median, comparator)
    val right = mergeSort(array, buffer, median + 1, end, comparator)

    val target = if (left === buffer) array else buffer

    // Merge.
    var leftIndex = start
    var rightIndex = median + 1
    for (i in start..end) {
        when {
            leftIndex <= median && rightIndex <= end -> {
                val leftValue = left[leftIndex]
                val rightValue = right[rightIndex]

                if (comparator.compare(leftValue, rightValue) <= 0) {
                    target[i] = leftValue
                    leftIndex++
                } else {
                    target[i] = rightValue
                    rightIndex++
                }
            }
            leftIndex <= median -> {
                target[i] = left[leftIndex]
                leftIndex++
            }
            else /* rightIndex <= end */ -> {
                target[i] = right[rightIndex]
                rightIndex++
            }
        }
    }

    return target
}

// FloatArray   =============================================================================
private fun partition(
        array: FloatArray, left: Int, right: Int): Int {
    var i = left
    var j = right
    val pivot = array[(left + right) / 2]
    while (i <= j) {
        while (array[i].compareTo(pivot) < 0)
            i++
        while (array[j].compareTo(pivot) > 0)
            j--
        if (i <= j) {
            val tmp = array[i]
            array[i] = array[j]
            array[j] = tmp
            i++
            j--
        }
    }
    return i
}

private fun quickSort(
        array: FloatArray, left: Int, right: Int) {
    val index = partition(array, left, right)
    if (left < index - 1)
        quickSort(array, left, index - 1)
    if (index < right)
        quickSort(array, index, right)
}

// DoubleArray  =============================================================================
private fun partition(
        array: DoubleArray, left: Int, right: Int): Int {
    var i = left
    var j = right
    val pivot = array[(left + right) / 2]
    while (i <= j) {
        while (array[i].compareTo(pivot) < 0)
            i++
        while (array[j].compareTo(pivot) > 0)
            j--
        if (i <= j) {
            val tmp = array[i]
            array[i] = array[j]
            array[j] = tmp
            i++
            j--
        }
    }
    return i
}

private fun quickSort(
        array: DoubleArray, left: Int, right: Int) {
    val index = partition(array, left, right)
    if (left < index - 1)
        quickSort(array, left, index - 1)
    if (index < right)
        quickSort(array, index, right)
}

// BooleanArray =============================================================================
private fun partition(
        array: BooleanArray, left: Int, right: Int): Int {
    var i = left
    var j = right
    val pivot = array[(left + right) / 2]
    while (i <= j) {
        while (array[i] < pivot)
            i++
        while (array[j] > pivot)
            j--
        if (i <= j) {
            val tmp = array[i]
            array[i] = array[j]
            array[j] = tmp
            i++
            j--
        }
    }
    return i
}

private fun quickSort(
        array: BooleanArray, left: Int, right: Int) {
    val index = partition(array, left, right)
    if (left < index - 1)
        quickSort(array, left, index - 1)
    if (index < right)
        quickSort(array, index, right)
}

// Interfaces   =============================================================================
/**
 * Sorts the subarray specified by [fromIndex] (inclusive) and [toIndex] (exclusive) parameters
 * using the merge sort algorithm with the given [comparator].
 */
internal fun <T> sortArrayWith(array: Array<out T>, fromIndex: Int, toIndex: Int, comparator: Comparator<T>) {
    if (fromIndex < toIndex - 1) {
        @Suppress("UNCHECKED_CAST")
        mergeSort(array as Array<T>, fromIndex, toIndex - 1, comparator)
    }
}

/**
 * Sorts a subarray of [Comparable] elements specified by [fromIndex] (inclusive) and
 * [toIndex] (exclusive) parameters using the merge sort algorithm.
 */
internal fun <T: Comparable<T>> sortArray(array: Array<out T>, fromIndex: Int, toIndex: Int) {
    if (fromIndex < toIndex - 1) {
        @Suppress("UNCHECKED_CAST")
        mergeSort(array as Array<T>, fromIndex, toIndex - 1)
    }
}

/**
 * Sorts the given array using qsort algorithm.
 */
internal fun sortArray(array: ByteArray, fromIndex: Int, toIndex: Int) = introSort(array, fromIndex, toIndex)
internal fun sortArray(array: ShortArray, fromIndex: Int, toIndex: Int) = introSort(array, fromIndex, toIndex)
internal fun sortArray(array: IntArray, fromIndex: Int, toIndex: Int) = introSort(array, fromIndex, toIndex)
internal fun sortArray(array: LongArray, fromIndex: Int, toIndex: Int) = introSort(array, fromIndex, toIndex)
internal fun sortArray(array: CharArray, fromIndex: Int, toIndex: Int) = introSort(array, fromIndex, toIndex)
internal fun sortArray(array: FloatArray, fromIndex: Int, toIndex: Int)   = quickSort(array, fromIndex, toIndex - 1)
internal fun sortArray(array: DoubleArray, fromIndex: Int, toIndex: Int)  = quickSort(array, fromIndex, toIndex - 1)
internal fun sortArray(array: BooleanArray, fromIndex: Int, toIndex: Int) = quickSort(array, fromIndex, toIndex - 1)

private const val INTRO_SORT_INSERTION_THRESHOLD = 24

// Kore: introsort for the integral element types (as C++'s std::sort): median-of-three quicksort that recurses into the
// smaller side, insertion sort for short ranges, heapsort when the recursion gets too deep (no quadratic worst case).
// The order is the same as the quicksort's (integers have a total order, and the sort is not stable either way).
private fun introSort(array: ByteArray, lo0: Int, hi0: Int, depth0: Int) {
    var lo = lo0
    var hi = hi0
    var depth = depth0
    while (hi - lo > INTRO_SORT_INSERTION_THRESHOLD) {
        if (depth == 0) {
            heapSort(array, lo, hi)
            return
        }
        depth--
        val a = array[lo]
        val b = array[(lo + hi) ushr 1]
        val c = array[hi]
        val pivot = if (a < b) (if (b < c) b else if (a < c) c else a) else (if (a < c) a else if (b < c) c else b)
        var i = lo
        var j = hi
        while (i <= j) {
            while (array[i] < pivot) i++
            while (array[j] > pivot) j--
            if (i <= j) {
                val tmp = array[i]
                array[i] = array[j]
                array[j] = tmp
                i++
                j--
            }
        }
        if (j - lo < hi - i) {
            introSort(array, lo, j, depth)
            lo = i
        } else {
            introSort(array, i, hi, depth)
            hi = j
        }
    }
    for (k in lo + 1..hi) {
        val value = array[k]
        var m = k - 1
        while (m >= lo && array[m] > value) {
            array[m + 1] = array[m]
            m--
        }
        array[m + 1] = value
    }
}

private fun heapSort(array: ByteArray, lo: Int, hi: Int) {
    val n = hi - lo + 1
    fun siftDown(start: Int, size: Int) {
        var root = start
        while (true) {
            var child = 2 * root + 1
            if (child >= size) return
            if (child + 1 < size && array[lo + child] < array[lo + child + 1]) child++
            if (array[lo + root] >= array[lo + child]) return
            val tmp = array[lo + root]
            array[lo + root] = array[lo + child]
            array[lo + child] = tmp
            root = child
        }
    }
    for (start in n / 2 - 1 downTo 0) siftDown(start, n)
    for (end in n - 1 downTo 1) {
        val tmp = array[lo]
        array[lo] = array[lo + end]
        array[lo + end] = tmp
        siftDown(0, end)
    }
}

private fun introSort(array: ByteArray, fromIndex: Int, toIndex: Int) {
    val n = toIndex - fromIndex
    if (n < 2) return
    introSort(array, fromIndex, toIndex - 1, 2 * (32 - (n - 1).countLeadingZeroBits()))
}

// Kore: introsort for the integral element types (as C++'s std::sort): median-of-three quicksort that recurses into the
// smaller side, insertion sort for short ranges, heapsort when the recursion gets too deep (no quadratic worst case).
// The order is the same as the quicksort's (integers have a total order, and the sort is not stable either way).
private fun introSort(array: ShortArray, lo0: Int, hi0: Int, depth0: Int) {
    var lo = lo0
    var hi = hi0
    var depth = depth0
    while (hi - lo > INTRO_SORT_INSERTION_THRESHOLD) {
        if (depth == 0) {
            heapSort(array, lo, hi)
            return
        }
        depth--
        val a = array[lo]
        val b = array[(lo + hi) ushr 1]
        val c = array[hi]
        val pivot = if (a < b) (if (b < c) b else if (a < c) c else a) else (if (a < c) a else if (b < c) c else b)
        var i = lo
        var j = hi
        while (i <= j) {
            while (array[i] < pivot) i++
            while (array[j] > pivot) j--
            if (i <= j) {
                val tmp = array[i]
                array[i] = array[j]
                array[j] = tmp
                i++
                j--
            }
        }
        if (j - lo < hi - i) {
            introSort(array, lo, j, depth)
            lo = i
        } else {
            introSort(array, i, hi, depth)
            hi = j
        }
    }
    for (k in lo + 1..hi) {
        val value = array[k]
        var m = k - 1
        while (m >= lo && array[m] > value) {
            array[m + 1] = array[m]
            m--
        }
        array[m + 1] = value
    }
}

private fun heapSort(array: ShortArray, lo: Int, hi: Int) {
    val n = hi - lo + 1
    fun siftDown(start: Int, size: Int) {
        var root = start
        while (true) {
            var child = 2 * root + 1
            if (child >= size) return
            if (child + 1 < size && array[lo + child] < array[lo + child + 1]) child++
            if (array[lo + root] >= array[lo + child]) return
            val tmp = array[lo + root]
            array[lo + root] = array[lo + child]
            array[lo + child] = tmp
            root = child
        }
    }
    for (start in n / 2 - 1 downTo 0) siftDown(start, n)
    for (end in n - 1 downTo 1) {
        val tmp = array[lo]
        array[lo] = array[lo + end]
        array[lo + end] = tmp
        siftDown(0, end)
    }
}

private fun introSort(array: ShortArray, fromIndex: Int, toIndex: Int) {
    val n = toIndex - fromIndex
    if (n < 2) return
    introSort(array, fromIndex, toIndex - 1, 2 * (32 - (n - 1).countLeadingZeroBits()))
}

// Kore: introsort for the integral element types (as C++'s std::sort): median-of-three quicksort that recurses into the
// smaller side, insertion sort for short ranges, heapsort when the recursion gets too deep (no quadratic worst case).
// The order is the same as the quicksort's (integers have a total order, and the sort is not stable either way).
private fun introSort(array: IntArray, lo0: Int, hi0: Int, depth0: Int) {
    var lo = lo0
    var hi = hi0
    var depth = depth0
    while (hi - lo > INTRO_SORT_INSERTION_THRESHOLD) {
        if (depth == 0) {
            heapSort(array, lo, hi)
            return
        }
        depth--
        val a = array[lo]
        val b = array[(lo + hi) ushr 1]
        val c = array[hi]
        val pivot = if (a < b) (if (b < c) b else if (a < c) c else a) else (if (a < c) a else if (b < c) c else b)
        var i = lo
        var j = hi
        while (i <= j) {
            while (array[i] < pivot) i++
            while (array[j] > pivot) j--
            if (i <= j) {
                val tmp = array[i]
                array[i] = array[j]
                array[j] = tmp
                i++
                j--
            }
        }
        if (j - lo < hi - i) {
            introSort(array, lo, j, depth)
            lo = i
        } else {
            introSort(array, i, hi, depth)
            hi = j
        }
    }
    for (k in lo + 1..hi) {
        val value = array[k]
        var m = k - 1
        while (m >= lo && array[m] > value) {
            array[m + 1] = array[m]
            m--
        }
        array[m + 1] = value
    }
}

private fun heapSort(array: IntArray, lo: Int, hi: Int) {
    val n = hi - lo + 1
    fun siftDown(start: Int, size: Int) {
        var root = start
        while (true) {
            var child = 2 * root + 1
            if (child >= size) return
            if (child + 1 < size && array[lo + child] < array[lo + child + 1]) child++
            if (array[lo + root] >= array[lo + child]) return
            val tmp = array[lo + root]
            array[lo + root] = array[lo + child]
            array[lo + child] = tmp
            root = child
        }
    }
    for (start in n / 2 - 1 downTo 0) siftDown(start, n)
    for (end in n - 1 downTo 1) {
        val tmp = array[lo]
        array[lo] = array[lo + end]
        array[lo + end] = tmp
        siftDown(0, end)
    }
}

private fun introSort(array: IntArray, fromIndex: Int, toIndex: Int) {
    val n = toIndex - fromIndex
    if (n < 2) return
    introSort(array, fromIndex, toIndex - 1, 2 * (32 - (n - 1).countLeadingZeroBits()))
}

// Kore: introsort for the integral element types (as C++'s std::sort): median-of-three quicksort that recurses into the
// smaller side, insertion sort for short ranges, heapsort when the recursion gets too deep (no quadratic worst case).
// The order is the same as the quicksort's (integers have a total order, and the sort is not stable either way).
private fun introSort(array: LongArray, lo0: Int, hi0: Int, depth0: Int) {
    var lo = lo0
    var hi = hi0
    var depth = depth0
    while (hi - lo > INTRO_SORT_INSERTION_THRESHOLD) {
        if (depth == 0) {
            heapSort(array, lo, hi)
            return
        }
        depth--
        val a = array[lo]
        val b = array[(lo + hi) ushr 1]
        val c = array[hi]
        val pivot = if (a < b) (if (b < c) b else if (a < c) c else a) else (if (a < c) a else if (b < c) c else b)
        var i = lo
        var j = hi
        while (i <= j) {
            while (array[i] < pivot) i++
            while (array[j] > pivot) j--
            if (i <= j) {
                val tmp = array[i]
                array[i] = array[j]
                array[j] = tmp
                i++
                j--
            }
        }
        if (j - lo < hi - i) {
            introSort(array, lo, j, depth)
            lo = i
        } else {
            introSort(array, i, hi, depth)
            hi = j
        }
    }
    for (k in lo + 1..hi) {
        val value = array[k]
        var m = k - 1
        while (m >= lo && array[m] > value) {
            array[m + 1] = array[m]
            m--
        }
        array[m + 1] = value
    }
}

private fun heapSort(array: LongArray, lo: Int, hi: Int) {
    val n = hi - lo + 1
    fun siftDown(start: Int, size: Int) {
        var root = start
        while (true) {
            var child = 2 * root + 1
            if (child >= size) return
            if (child + 1 < size && array[lo + child] < array[lo + child + 1]) child++
            if (array[lo + root] >= array[lo + child]) return
            val tmp = array[lo + root]
            array[lo + root] = array[lo + child]
            array[lo + child] = tmp
            root = child
        }
    }
    for (start in n / 2 - 1 downTo 0) siftDown(start, n)
    for (end in n - 1 downTo 1) {
        val tmp = array[lo]
        array[lo] = array[lo + end]
        array[lo + end] = tmp
        siftDown(0, end)
    }
}

private fun introSort(array: LongArray, fromIndex: Int, toIndex: Int) {
    val n = toIndex - fromIndex
    if (n < 2) return
    introSort(array, fromIndex, toIndex - 1, 2 * (32 - (n - 1).countLeadingZeroBits()))
}

// Kore: introsort for the integral element types (as C++'s std::sort): median-of-three quicksort that recurses into the
// smaller side, insertion sort for short ranges, heapsort when the recursion gets too deep (no quadratic worst case).
// The order is the same as the quicksort's (integers have a total order, and the sort is not stable either way).
private fun introSort(array: CharArray, lo0: Int, hi0: Int, depth0: Int) {
    var lo = lo0
    var hi = hi0
    var depth = depth0
    while (hi - lo > INTRO_SORT_INSERTION_THRESHOLD) {
        if (depth == 0) {
            heapSort(array, lo, hi)
            return
        }
        depth--
        val a = array[lo]
        val b = array[(lo + hi) ushr 1]
        val c = array[hi]
        val pivot = if (a < b) (if (b < c) b else if (a < c) c else a) else (if (a < c) a else if (b < c) c else b)
        var i = lo
        var j = hi
        while (i <= j) {
            while (array[i] < pivot) i++
            while (array[j] > pivot) j--
            if (i <= j) {
                val tmp = array[i]
                array[i] = array[j]
                array[j] = tmp
                i++
                j--
            }
        }
        if (j - lo < hi - i) {
            introSort(array, lo, j, depth)
            lo = i
        } else {
            introSort(array, i, hi, depth)
            hi = j
        }
    }
    for (k in lo + 1..hi) {
        val value = array[k]
        var m = k - 1
        while (m >= lo && array[m] > value) {
            array[m + 1] = array[m]
            m--
        }
        array[m + 1] = value
    }
}

private fun heapSort(array: CharArray, lo: Int, hi: Int) {
    val n = hi - lo + 1
    fun siftDown(start: Int, size: Int) {
        var root = start
        while (true) {
            var child = 2 * root + 1
            if (child >= size) return
            if (child + 1 < size && array[lo + child] < array[lo + child + 1]) child++
            if (array[lo + root] >= array[lo + child]) return
            val tmp = array[lo + root]
            array[lo + root] = array[lo + child]
            array[lo + child] = tmp
            root = child
        }
    }
    for (start in n / 2 - 1 downTo 0) siftDown(start, n)
    for (end in n - 1 downTo 1) {
        val tmp = array[lo]
        array[lo] = array[lo + end]
        array[lo + end] = tmp
        siftDown(0, end)
    }
}

private fun introSort(array: CharArray, fromIndex: Int, toIndex: Int) {
    val n = toIndex - fromIndex
    if (n < 2) return
    introSort(array, fromIndex, toIndex - 1, 2 * (32 - (n - 1).countLeadingZeroBits()))
}
