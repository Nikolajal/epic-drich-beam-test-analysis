#pragma once

/**
 * @file util/bit_ops.h
 * @brief 32-bit mask manipulation helpers — encode/decode individual bits.
 *
 * Pure standard-library code, no ROOT dependency.  Used throughout the
 * framework for per-device participant and dead-channel masks.
 */

#include <cassert>
#include <cstdint>
#include <vector>

/**
 * @brief Encode a single bit into a 32-bit mask.
 * @param active_bit Index of the bit to set (0..31).  Out-of-range input
 *                   triggers a debug-build assert; release builds silently
 *                   return 0 (CODE_REVIEW §5.7).
 * @return 32-bit mask with only that bit set, or 0 if @p active_bit ≥ 32.
 */
inline uint32_t encode_bit(uint8_t active_bit)
{
    assert(active_bit < 32 && "encode_bit: active_bit must be < 32");
    return (active_bit < 32) ? (1u << active_bit) : 0;
}

/**
 * @brief Encode multiple bits into a 32-bit mask.
 * @param active_bits Vector of bit indices to set (0..31).  Out-of-range
 *                    indices trigger a debug-build assert and are silently
 *                    skipped in release builds.
 * @return 32-bit mask with all in-range specified bits set.
 */
inline uint32_t encode_bits(const std::vector<uint8_t> &active_bits)
{
    uint32_t mask = 0;
    for (uint8_t bit : active_bits)
    {
        assert(bit < 32 && "encode_bits: every bit index must be < 32");
        if (bit < 32)
            mask |= (1u << bit);
    }
    return mask;
}

/**
 * @brief Count trailing zeros — uses the compiler intrinsic when available
 *        (constant-time), with a portable fallback (CODE_REVIEW §5.8).
 * @param mask 32-bit mask
 * @return Index of least significant set bit, 32 if mask is 0
 */
inline uint8_t count_trailing_zeros(uint32_t mask)
{
    if (mask == 0)
        return 32;
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<uint8_t>(__builtin_ctz(mask));
#else
    uint8_t count = 0;
    while ((mask & 1u) == 0)
    {
        mask >>= 1;
        ++count;
    }
    return count;
#endif
}

/**
 * @brief Decode a 32-bit mask into the indices of set bits.
 *
 * Uses the `mask &= mask - 1` Kernighan trick to clear the lowest set bit
 * in one operation per iteration — total work is O(popcount(mask)) rather
 * than O(highest-set-bit) (CODE_REVIEW §5.8).
 *
 * @param mask 32-bit mask
 * @return Vector of indices where bits are set
 */
inline std::vector<uint8_t> decode_bits(uint32_t mask)
{
    std::vector<uint8_t> result;
    result.reserve(32);
    while (mask)
    {
        result.push_back(count_trailing_zeros(mask));
        mask &= mask - 1; // clear lowest set bit
    }
    return result;
}
