// Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by
// the Apache 2.0 license that can be found in the license/LICENSE.txt file.

#include "MemorySearch.hpp"

#include <cstdint>
#include <vector>

#include "gtest/gtest.h"

using namespace kotlin;

namespace {

template <typename Unit>
size_t naiveFind(const std::vector<Unit>& units, size_t from, Unit value) {
    for (size_t i = from; i < units.size(); ++i) {
        if (units[i] == value) return i;
    }
    return units.size();
}

} // namespace

TEST(MemorySearchTest, FirstMismatchByte) {
    for (size_t n = 0; n < 100; ++n) {
        // Offsets make the inputs unaligned.
        std::vector<uint8_t> a(n + 3), b(n + 5);
        for (size_t i = 0; i < n; ++i) a[i + 3] = b[i + 5] = static_cast<uint8_t>(i * 7 + 1);
        EXPECT_EQ(firstMismatchByte(a.data() + 3, b.data() + 5, n), n);
        for (size_t at = 0; at < n; ++at) {
            b[at + 5] ^= 0x80;
            EXPECT_EQ(firstMismatchByte(a.data() + 3, b.data() + 5, n), at) << n << " " << at;
            // Only the first mismatch counts.
            if (at + 1 < n) {
                b[n - 1 + 5] ^= 0x01;
                EXPECT_EQ(firstMismatchByte(a.data() + 3, b.data() + 5, n), at) << n << " " << at;
                b[n - 1 + 5] ^= 0x01;
            }
            b[at + 5] ^= 0x80;
        }
    }
}

TEST(MemorySearchTest, FindUnit16) {
    for (size_t n = 0; n < 80; ++n) {
        std::vector<uint16_t> units(n);
        for (size_t i = 0; i < n; ++i) units[i] = static_cast<uint16_t>('a' + i % 26);
        for (size_t from = 0; from <= n; ++from) {
            for (uint16_t value : {uint16_t('a'), uint16_t('z'), uint16_t('q'), uint16_t(0x161), uint16_t(0x6161)}) {
                EXPECT_EQ(findUnit<uint16_t>(units.data(), from, n, value), naiveFind(units, from, value))
                        << n << " " << from << " " << value;
            }
        }
    }
}

TEST(MemorySearchTest, FindUnit8) {
    for (size_t n = 0; n < 80; ++n) {
        std::vector<uint8_t> units(n);
        for (size_t i = 0; i < n; ++i) units[i] = static_cast<uint8_t>(i % 3 == 0 ? 0xE1 : 'a' + i % 26);
        for (size_t from = 0; from <= n; ++from) {
            for (uint8_t value : {uint8_t('a'), uint8_t('z'), uint8_t(0xE1), uint8_t(0)}) {
                EXPECT_EQ(findUnit<uint8_t>(units.data(), from, n, value), naiveFind(units, from, value))
                        << n << " " << from << " " << int(value);
            }
        }
    }
}
