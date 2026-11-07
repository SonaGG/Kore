// Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by
// the Apache 2.0 license that can be found in the license/LICENSE.txt file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace kotlin {

// Kore: vectorized helpers for comparisons and searches over string and primitive array contents. The C library ones
// are either slow (MinGW's msvcrt `memcmp` compares bytewise) or do not exist for 16-bit units.

// The index of the first byte where `a` and `b` differ, or `n` if they are equal.
inline size_t firstMismatchByte(const void* a, const void* b, size_t n) noexcept {
    auto* x = static_cast<const uint8_t*>(a);
    auto* y = static_cast<const uint8_t*>(b);
    size_t i = 0;
#if defined(__SSE2__)
    for (; i + 32 <= n; i += 32) {
        __m128i x0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i));
        __m128i y0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(y + i));
        __m128i x1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i + 16));
        __m128i y1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(y + i + 16));
        uint32_t equal0 = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(x0, y0)));
        uint32_t equal1 = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(x1, y1)));
        uint32_t equal = equal0 | (equal1 << 16);
        if (equal != 0xFFFFFFFFu) return i + static_cast<size_t>(__builtin_ctz(~equal));
    }
#endif
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    for (; i + 8 <= n; i += 8) {
        uint64_t u, v;
        memcpy(&u, x + i, 8);
        memcpy(&v, y + i, 8);
        if (u != v) return i + static_cast<size_t>(__builtin_ctzll(u ^ v) >> 3);
    }
#endif
    for (; i < n; ++i) {
        if (x[i] != y[i]) return i;
    }
    return n;
}

// The index of the first unit equal to `value` in `units[from, n)`, or `n` if there is none.
template <typename Unit>
inline size_t findUnit(const Unit* units, size_t from, size_t n, Unit value) noexcept {
    static_assert(sizeof(Unit) == 1 || sizeof(Unit) == 2, "Only bytes and 16-bit units");
    size_t i = from;
#if defined(__SSE2__)
    if (i + 16 / sizeof(Unit) <= n) {
        __m128i needle = sizeof(Unit) == 1 ? _mm_set1_epi8(static_cast<char>(value)) : _mm_set1_epi16(static_cast<short>(value));
        constexpr size_t step = 32 / sizeof(Unit);
        for (; i + step <= n; i += step) {
            __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(units + i));
            __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(units + i) + 1);
            __m128i ea = sizeof(Unit) == 1 ? _mm_cmpeq_epi8(a, needle) : _mm_cmpeq_epi16(a, needle);
            __m128i eb = sizeof(Unit) == 1 ? _mm_cmpeq_epi8(b, needle) : _mm_cmpeq_epi16(b, needle);
            uint32_t found = static_cast<uint32_t>(_mm_movemask_epi8(ea)) | (static_cast<uint32_t>(_mm_movemask_epi8(eb)) << 16);
            if (found != 0) return i + static_cast<size_t>(__builtin_ctz(found)) / sizeof(Unit);
        }
    }
#endif
    for (; i < n; ++i) {
        if (units[i] == value) return i;
    }
    return n;
}

// The index of the first occurrence of `pattern[0, m)` (m > 0) in `units[from, n)`, or `n` if there is none: candidates
// are found by their first unit, a vector at a time.
template <typename Unit>
inline size_t findUnits(const Unit* units, size_t from, size_t n, const Unit* pattern, size_t m) noexcept {
    if (m > n) return n;
    size_t last = n - m + 1; // Candidates start before `last`.
    for (size_t i = from; i < last; ++i) {
        i = findUnit<Unit>(units, i, last, pattern[0]);
        if (i == last) break;
        if (firstMismatchByte(units + i + 1, pattern + 1, (m - 1) * sizeof(Unit)) == (m - 1) * sizeof(Unit)) return i;
    }
    return n;
}

} // namespace kotlin
