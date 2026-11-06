/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace kotlin {

// Kore: decimal formatting of integers, as `snprintf("%lld")` would produce (an optional '-' then digits), without the
// cost of the printf machinery. Writes the characters (at most 20) to `out`, returns their number. The digit count is
// computed first, so the digits go straight to their place, two at a time.
template <typename Char>
inline size_t formatDecimalTo(int64_t value, Char* out) noexcept {
    static constexpr char kPairs[] =
            "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
            "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
            "8081828384858687888990919293949596979899";
    // Work with the magnitude in unsigned arithmetic so that INT64_MIN is handled correctly.
    uint64_t magnitude = value < 0 ? ~static_cast<uint64_t>(value) + 1 : static_cast<uint64_t>(value);
    size_t digits = 1;
    for (uint64_t bound = 10; digits < 20 && magnitude >= bound; bound *= 10) ++digits;
    size_t length = digits;
    if (value < 0) {
        *out++ = static_cast<Char>('-');
        ++length;
    }
    Char* end = out + digits;
    while (magnitude >= 100) {
        auto pair = static_cast<size_t>(magnitude % 100) * 2;
        magnitude /= 100;
        *--end = static_cast<Char>(kPairs[pair + 1]);
        *--end = static_cast<Char>(kPairs[pair]);
    }
    if (magnitude >= 10) {
        auto pair = static_cast<size_t>(magnitude) * 2;
        *--end = static_cast<Char>(kPairs[pair + 1]);
        *--end = static_cast<Char>(kPairs[pair]);
    } else {
        *--end = static_cast<Char>('0' + magnitude);
    }
    return length;
}

// Like `formatDecimalTo`, followed by a terminating '\0' (21 bytes needed in `buffer`).
inline size_t formatDecimal(int64_t value, char* buffer) noexcept {
    size_t length = formatDecimalTo(value, buffer);
    buffer[length] = '\0';
    return length;
}

} // namespace kotlin
