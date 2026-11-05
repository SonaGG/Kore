/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#include "DecimalFormat.hpp"

#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include "gtest/gtest.h"

using namespace kotlin;

namespace {

std::string viaSnprintf(int64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    return buffer;
}

std::string viaFormatDecimal(int64_t value) {
    char buffer[21];
    auto length = formatDecimal(value, buffer);
    EXPECT_EQ(length, std::strlen(buffer));
    return buffer;
}

} // namespace

TEST(DecimalFormatTest, MatchesSnprintf) {
    const int64_t special[] = {
            0, 1, -1, 9, 10, -10, 99, 100, 127, -128, 32767, -32768,
            std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min(),
            std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min(),
            std::numeric_limits<int64_t>::max() - 1, std::numeric_limits<int64_t>::min() + 1,
            1000000000000000000LL, -999999999999999999LL,
    };
    for (auto value : special) {
        EXPECT_EQ(viaFormatDecimal(value), viaSnprintf(value)) << value;
    }
    for (int64_t value = -100000; value <= 100000; value += 7) {
        ASSERT_EQ(viaFormatDecimal(value), viaSnprintf(value)) << value;
    }
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 100000; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        auto value = static_cast<int64_t>(x);
        ASSERT_EQ(viaFormatDecimal(value), viaSnprintf(value)) << value;
    }
}
