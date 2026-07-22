// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace MusicPlayerLibrary
{
    template <typename UnsignedInteger>
    [[nodiscard]] constexpr UnsignedInteger DecodeLittleEndian(
        const std::span<const std::uint8_t, sizeof(UnsignedInteger)> bytes) noexcept
    {
        static_assert(std::is_integral_v<UnsignedInteger>);
        static_assert(std::is_unsigned_v<UnsignedInteger>);
        static_assert(!std::is_same_v<std::remove_cv_t<UnsignedInteger>, bool>);

        UnsignedInteger value = 0;
        for (std::size_t index = 0; index < bytes.size(); ++index)
        {
            value |= static_cast<UnsignedInteger>(bytes[index]) << (index * 8);
        }
        return value;
    }

    template <typename UnsignedInteger>
    [[nodiscard]] constexpr std::array<std::uint8_t, sizeof(UnsignedInteger)> EncodeLittleEndian(
        const UnsignedInteger value) noexcept
    {
        static_assert(std::is_integral_v<UnsignedInteger>);
        static_assert(std::is_unsigned_v<UnsignedInteger>);
        static_assert(!std::is_same_v<std::remove_cv_t<UnsignedInteger>, bool>);

        std::array<std::uint8_t, sizeof(UnsignedInteger)> bytes{};
        for (std::size_t index = 0; index < bytes.size(); ++index)
        {
            bytes[index] = static_cast<std::uint8_t>(value >> (index * 8));
        }
        return bytes;
    }
}
