/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

// Kore: Base64 encoding and decoding of whole blocks with SSSE3 (12 bytes to 16 symbols and back), for
// `kotlin.io.encoding.Base64`, which handles the rest (padding, MIME line breaks, errors at their exact index). The
// algorithms are those of Wojciech Muła and Daniel Lemire ("Faster Base64 Encoding and Decoding using AVX2
// Instructions", 2018), on 128-bit vectors.

#include <cstdint>
#include <cstring>

#include "Memory.h"
#include "Natives.h"
#include "Types.h"

#if defined(__x86_64__)
#include <cpuid.h>
#include <immintrin.h>

namespace {

bool hasSsse3() noexcept {
    static const bool value = [] {
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
        return (c & bit_SSSE3) != 0;
    }();
    return value;
}

// 12 bytes (of 16 readable) to 16 symbols.
__attribute__((target("ssse3"))) inline void encodeBlock(const uint8_t* in, uint8_t* out, __m128i shiftTable) noexcept {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in));
    // Each 32-bit lane gets bytes [b1, b0, b2, b1] of its 3 input bytes.
    v = _mm_shuffle_epi8(v, _mm_set_epi8(10, 11, 9, 10, 7, 8, 6, 7, 4, 5, 3, 4, 1, 2, 0, 1));
    // The four 6-bit indices of each lane, one per byte.
    __m128i t0 = _mm_and_si128(v, _mm_set1_epi32(0x0fc0fc00));
    __m128i t1 = _mm_mulhi_epu16(t0, _mm_set1_epi32(0x04000040));
    __m128i t2 = _mm_and_si128(v, _mm_set1_epi32(0x003f03f0));
    __m128i t3 = _mm_mullo_epi16(t2, _mm_set1_epi32(0x01000010));
    __m128i indices = _mm_or_si128(t1, t3);
    // Indices to symbols: 0..25 -> 13, 26..51 -> 0, 52..61 -> 1..10, 62 -> 11, 63 -> 12, then the offset of that range.
    __m128i range = _mm_subs_epu8(indices, _mm_set1_epi8(51));
    __m128i less = _mm_cmpgt_epi8(_mm_set1_epi8(26), indices);
    range = _mm_or_si128(range, _mm_and_si128(less, _mm_set1_epi8(13)));
    __m128i symbols = _mm_add_epi8(_mm_shuffle_epi8(shiftTable, range), indices);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out), symbols);
}

// 16 symbols to 12 bytes; false (nothing written) if any is not one of the alphabet's.
__attribute__((target("ssse3"))) inline bool decodeBlock(const uint8_t* in, uint8_t* out, char symbol62, char symbol63) noexcept {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in));
    auto range = [v](char low, char high) {
        return _mm_and_si128(_mm_cmpgt_epi8(v, _mm_set1_epi8(static_cast<char>(low - 1))),
                             _mm_cmplt_epi8(v, _mm_set1_epi8(static_cast<char>(high + 1))));
    };
    __m128i upper = range('A', 'Z');
    __m128i lower = range('a', 'z');
    __m128i digits = range('0', '9');
    __m128i is62 = _mm_cmpeq_epi8(v, _mm_set1_epi8(symbol62));
    __m128i is63 = _mm_cmpeq_epi8(v, _mm_set1_epi8(symbol63));
    __m128i valid = _mm_or_si128(_mm_or_si128(upper, lower), _mm_or_si128(digits, _mm_or_si128(is62, is63)));
    if (_mm_movemask_epi8(valid) != 0xFFFF) return false;
    __m128i shift = _mm_or_si128(
            _mm_or_si128(_mm_and_si128(upper, _mm_set1_epi8(-'A')), _mm_and_si128(lower, _mm_set1_epi8(26 - 'a'))),
            _mm_or_si128(_mm_and_si128(digits, _mm_set1_epi8(52 - '0')),
                         _mm_or_si128(_mm_and_si128(is62, _mm_set1_epi8(static_cast<char>(62 - symbol62))),
                                      _mm_and_si128(is63, _mm_set1_epi8(static_cast<char>(63 - symbol63))))));
    __m128i values = _mm_add_epi8(v, shift);
    // Per 32-bit lane: (v0 << 6 | v1) and (v2 << 6 | v3), then their 24 bits, whose bytes are output high first.
    __m128i pairs = _mm_maddubs_epi16(values, _mm_set1_epi32(0x01400140));
    __m128i lanes = _mm_madd_epi16(pairs, _mm_set1_epi32(0x00011000));
    __m128i bytes = _mm_shuffle_epi8(lanes, _mm_setr_epi8(2, 1, 0, 6, 5, 4, 10, 9, 8, 14, 13, 12, -1, -1, -1, -1));
    _mm_storel_epi64(reinterpret_cast<__m128i*>(out), bytes);
    uint32_t last = static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_srli_si128(bytes, 8)));
    std::memcpy(out + 8, &last, 4);
    return true;
}

} // namespace

// The source bytes [startIndex, endIndex) encoded, a whole number of 12-byte blocks of them, into `destination` from
// `destinationOffset`; returns the number of source bytes encoded.
extern "C" __attribute__((target("ssse3"))) KInt Kotlin_Base64_encodeBlocks(
        KConstRef source, KInt startIndex, KInt endIndex, KRef destination, KInt destinationOffset, KBoolean urlSafe) {
    if (!hasSsse3()) return 0;
    auto* in = reinterpret_cast<const uint8_t*>(ByteArrayAddressOfElementAt(source->array(), 0));
    auto* out = reinterpret_cast<uint8_t*>(ByteArrayAddressOfElementAt(destination->array(), 0)) + destinationOffset;
    __m128i shiftTable = _mm_setr_epi8('a' - 26, '0' - 52, '0' - 52, '0' - 52, '0' - 52, '0' - 52, '0' - 52, '0' - 52,
                                       '0' - 52, '0' - 52, '0' - 52, static_cast<char>((urlSafe ? '-' : '+') - 62),
                                       static_cast<char>((urlSafe ? '_' : '/') - 63), 'A', 0, 0);
    KInt index = startIndex;
    // Blocks read 16 bytes.
    for (; index + 16 <= endIndex; index += 12, out += 16) encodeBlock(in + index, out, shiftTable);
    return index - startIndex;
}

// The symbols [startIndex, endIndex) decoded, a whole number of 16-symbol blocks of them up to the first block with
// anything else than symbols of the alphabet, into `destination` from `destinationOffset`; returns the number of symbols
// decoded.
extern "C" __attribute__((target("ssse3"))) KInt Kotlin_Base64_decodeBlocks(
        KConstRef source, KInt startIndex, KInt endIndex, KRef destination, KInt destinationOffset, KBoolean urlSafe) {
    if (!hasSsse3()) return 0;
    auto* in = reinterpret_cast<const uint8_t*>(ByteArrayAddressOfElementAt(source->array(), 0));
    auto* out = reinterpret_cast<uint8_t*>(ByteArrayAddressOfElementAt(destination->array(), 0)) + destinationOffset;
    char symbol62 = urlSafe ? '-' : '+';
    char symbol63 = urlSafe ? '_' : '/';
    KInt index = startIndex;
    for (; index + 16 <= endIndex; index += 16, out += 12) {
        if (!decodeBlock(in + index, out, symbol62, symbol63)) break;
    }
    return index - startIndex;
}

#else

extern "C" KInt Kotlin_Base64_encodeBlocks(KConstRef, KInt, KInt, KRef, KInt, KBoolean) {
    return 0;
}

extern "C" KInt Kotlin_Base64_decodeBlocks(KConstRef, KInt, KInt, KRef, KInt, KBoolean) {
    return 0;
}

#endif
