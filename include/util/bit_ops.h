#pragma once

/**
 * @file util/bit_ops.h
 * @brief 32-bit mask manipulation helpers — encode/decode individual bits.
 *
 * Pure standard-library code, no ROOT dependency.  Used throughout the
 * framework for per-device participant and dead-channel masks.
 */

#include <cstdint>
#include <vector>

/**
 * @brief Encode a single bit into a 32-bit mask.
 * @param active_bit Index of the bit to set (0..31)
 * @return 32-bit mask with only that bit set
 */
inline uint32_t encode_bit(uint8_t active_bit)
{
    return (active_bit < 32) ? (1u << active_bit) : 0;
}

/**
 * @brief Encode multiple bits into a 32-bit mask.
 * @param active_bits Vector of bit indices to set (0..31)
 * @return 32-bit mask with all specified bits set
 */
inline uint32_t encode_bits(const std::vector<uint8_t> &active_bits)
{
    uint32_t mask = 0;
    for (uint8_t bit : active_bits)
        if (bit < 32)
            mask |= (1u << bit);
    return mask;
}

/**
 * @brief Count trailing zeros (portable C++17).
 * @param mask 32-bit mask
 * @return Index of least significant set bit, 32 if mask is 0
 */
inline uint8_t count_trailing_zeros(uint32_t mask)
{
    if (mask == 0)
        return 32;
    uint8_t count = 0;
    while ((mask & 1u) == 0)
    {
        mask >>= 1;
        ++count;
    }
    return count;
}

/**
 * @brief Decode a 32-bit mask into the indices of set bits.
 * @param mask 32-bit mask
 * @return Vector of indices where bits are set
 */
inline std::vector<uint8_t> decode_bits(uint32_t mask)
{
    std::vector<uint8_t> result;
    result.reserve(32);
    while (mask)
    {
        uint8_t bit = count_trailing_zeros(mask);
        result.push_back(bit);
        mask &= ~(1u << bit); // clear that bit
    }
    return result;
}
