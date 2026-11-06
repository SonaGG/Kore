/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace kotlin {

// Kore: `memmove` for array copies (which often overlap: shifting elements in place, e.g. gap buffers). The C library
// of some targets (MinGW's msvcrt) moves overlapping ranges slowly; this copies 64 bytes per step with unaligned
// 16-byte loads and stores, in the direction that reads every block before it is overwritten.
inline void moveMemory(void* destination, const void* source, size_t bytes) noexcept {
#if defined(__SSE2__)
    auto* d = static_cast<uint8_t*>(destination);
    auto* s = static_cast<const uint8_t*>(source);
    if (bytes < 64 || d == s) {
        std::memmove(destination, source, bytes);
        return;
    }
    if (d < s) {
        // Forward: each step loads its 64 bytes before storing them, at or below where they were read.
        size_t i = 0;
        for (; i + 64 <= bytes; i += 64) {
            __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i));
            __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i + 16));
            __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i + 32));
            __m128i e = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i + 48));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i), a);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i + 16), b);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i + 32), c);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i + 48), e);
        }
        if (i < bytes) std::memmove(d + i, s + i, bytes - i);
    } else {
        // Backward: the same from the end, so that stores land at or above the blocks still to be read.
        size_t i = bytes;
        for (; i >= 64; i -= 64) {
            __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i - 64));
            __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i - 48));
            __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i - 32));
            __m128i e = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i - 16));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i - 64), a);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i - 48), b);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i - 32), c);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i - 16), e);
        }
        if (i > 0) std::memmove(d, s, i);
    }
#else
    std::memmove(destination, source, bytes);
#endif
}

} // namespace kotlin
