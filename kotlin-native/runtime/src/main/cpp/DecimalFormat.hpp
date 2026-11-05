/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace kotlin {

// Kore: decimal formatting of integers, as `snprintf("%lld")` would produce (an optional '-' then digits), without the
// cost of the printf machinery. Writes at most 20 characters and a terminating '\0' into `buffer` (21 bytes needed),
// returns the number of characters written, excluding the terminator.
inline size_t formatDecimal(int64_t value, char* buffer) noexcept {
    // Work with the magnitude in unsigned arithmetic so that INT64_MIN is handled correctly.
    uint64_t magnitude = value < 0 ? ~static_cast<uint64_t>(value) + 1 : static_cast<uint64_t>(value);
    char digits[20];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    size_t length = 0;
    if (value < 0) buffer[length++] = '-';
    while (count > 0) buffer[length++] = digits[--count];
    buffer[length] = '\0';
    return length;
}

} // namespace kotlin
